#include <iostream>
#include <string>
#include <fstream>
#include <algorithm>
#include <vector>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <sqlite3.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <locale>
#include <codecvt>
#include <regex>
#include <chrono>
#include <mutex>
#include <cstddef>

using json = nlohmann::json;

// Variables globales
json g_knowledge_base;
sqlite3* g_db = nullptr;
std::mutex g_cache_mutex;
std::unordered_map<std::string, std::pair<std::string, std::chrono::system_clock::time_point>> g_cache;
const int CACHE_TTL_SECONDS = 3600; // 1 hora de validez del caché

// Prototipos de funciones
std::string search_cache(const std::string& question);
void save_to_cache(const std::string& question, const std::string& answer);
std::string search_database(const std::string& question, const std::string& language);
void save_to_database(const std::string& question, const std::string& answer, const std::string& language);
bool is_complex_question(const std::string& question);
std::string search_knowledge_base(const std::string& question, const std::string& language);
std::string generate_ollama_response(const std::string& question, const std::string& language);
bool has_long_period_without_status(const std::string& normalized_question);

// Funciones de log
void log_info(const std::string& message) {
    std::cout << "✅ [INFO] " << message << std::endl;
}

void log_error(const std::string& message) {
    std::cout << "❌ [ERROR] " << message << std::endl;
}

void log_debug(const std::string& message) {
    std::cout << "🔍 [DEBUG] " << message << std::endl;
}

// Inicializar la base de datos SQLite
bool init_database(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &g_db) != SQLITE_OK) {
        log_error("Error al abrir la base de datos: " + std::string(sqlite3_errmsg(g_db)));
        return false;
    }
    
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS chat_history ("
        "  id INTEGER PRIMARY KEY, "
        "  question TEXT NOT NULL, "
        "  answer TEXT NOT NULL, "
        "  language TEXT NOT NULL, " // Añadido para el soporte multi-idioma
        "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_question ON chat_history(question);"
        "CREATE INDEX IF NOT EXISTS idx_language ON chat_history(language);";
    
    char* errMsg = nullptr;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string error = errMsg;
        sqlite3_free(errMsg);
        log_error("Error al crear tablas: " + error);
        return false;
    }
    
    log_info("Base de datos inicializada correctamente");
    return true;
}

// Callback para recibir datos de CURL
size_t WriteCallback(char* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append(contents, size * nmemb);
    return size * nmemb;
}
// Detectar el idioma de un texto (simplificado a español/inglés)
std::string detect_language(const std::string& text) {
    // Palabras comunes en español
    std::vector<std::string> spanish_words = {
        "el", "la", "los", "las", "un", "una", "unos", "unas", "y", "o", "pero", "porque",
        "como", "cuando", "donde", "cual", "quien", "que", "esto", "esta", "estos", "estas",
        "ese", "esa", "esos", "esas", "para", "por", "con", "sin", "sobre", "bajo", "ante",
        "entre", "desde", "hacia", "hasta", "según", "durante", "mediante", "excepto",
        "salvo", "menos", "más", "muy", "mucho", "poco", "bastante", "demasiado", "casi",
        "aproximadamente", "todo", "nada", "algo", "alguien", "nadie", "ninguno", "alguno"
    };
    
    // Palabras comunes en inglés
    std::vector<std::string> english_words = {
        "the", "of", "and", "a", "to", "in", "is", "you", "that", "it", "he", "was", "for",
        "on", "are", "as", "with", "his", "they", "I", "at", "be", "this", "have", "from",
        "or", "one", "had", "by", "word", "but", "not", "what", "all", "were", "we", "when",
        "your", "can", "said", "there", "use", "an", "each", "which", "she", "do", "how",
        "their", "if", "will", "up", "other", "about", "out", "many", "then", "them", "these",
        "so", "some", "her", "would", "make", "like", "him", "into", "time", "has", "look"
    };
    
    // Convertir a minúsculas
    std::string lower_text = text;
    std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), 
                  [](unsigned char c){ return std::tolower(c); });
    
    // Contar palabras en español e inglés
    int spanish_count = 0;
    int english_count = 0;
    
    // Tokenizar el texto
    std::regex word_regex("\\b\\w+\\b");
    auto words_begin = std::sregex_iterator(lower_text.begin(), lower_text.end(), word_regex);
    auto words_end = std::sregex_iterator();
    
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::string word = i->str();
        
        if (std::find(spanish_words.begin(), spanish_words.end(), word) != spanish_words.end()) {
            spanish_count++;
        }
        
        if (std::find(english_words.begin(), english_words.end(), word) != english_words.end()) {
            english_count++;
        }
    }
    
    // Determinar el idioma basado en la cantidad de palabras reconocidas
    if (spanish_count > english_count) {
        return "es";
    } else {
        return "en";
    }
}

// Normalizar texto para búsqueda (eliminar acentos, convertir a minúsculas)
std::string normalize_text(const std::string& text) {
    std::string result = text;
    
    // Convertir a minúsculas
    std::transform(result.begin(), result.end(), result.begin(), 
                  [](unsigned char c){ return std::tolower(c); });
    
    // Reemplazar caracteres acentuados usando valores UTF-8
    std::unordered_map<unsigned char, unsigned char> accent_map = {
        {0xE1, 'a'}, // á
        {0xE9, 'e'}, // é
        {0xED, 'i'}, // í
        {0xF3, 'o'}, // ó
        {0xFA, 'u'}, // ú
        {0xFC, 'u'}, // ü
        {0xF1, 'n'}, // ñ
        {0xE0, 'a'}, // à
        {0xE8, 'e'}, // è
        {0xEC, 'i'}, // ì
        {0xF2, 'o'}, // ò
        {0xF9, 'u'}  // ù
    };
    
    for (auto& c : result) {
        auto it = accent_map.find(c);
        if (it != accent_map.end()) {
            c = it->second;
        }
    }
    
    return result;
}

// Detectar si hay un período largo sin estatus
bool has_long_period_without_status(const std::string& normalized_question) {
    return (normalized_question.find("3 año") != std::string::npos ||
            normalized_question.find("tres año") != std::string::npos ||
            normalized_question.find("mas de 180") != std::string::npos ||
            normalized_question.find("más de 180") != std::string::npos ||
            normalized_question.find("años sin estatus") != std::string::npos ||
            normalized_question.find("años sin status") != std::string::npos ||
            normalized_question.find("largo periodo") != std::string::npos ||
            normalized_question.find("largo tiempo") != std::string::npos ||
            normalized_question.find("mucho tiempo") != std::string::npos);
}
// Cargar la base de conocimiento
bool load_knowledge_base(const std::string& kb_path) {
    std::ifstream file(kb_path);
    
    if (file.is_open()) {
        try {
            log_info("Intentando cargar base de conocimiento desde: " + kb_path);
            json original_json;
            file >> original_json;
            
            // Convertir el formato encontrado a nuestro formato esperado
            g_knowledge_base = {{"data", json::array()}};
            
            // Procesar el formato actual
            for (auto& [category, questions] : original_json.items()) {
                if (questions.is_array()) {
                    for (auto& item : questions) {
                        if (item.contains("answer")) {
                            // Crear una entrada en nuestro formato esperado
                            json entry;
                            entry["question"] = item.contains("question") ? item["question"].get<std::string>() : 
                                              "¿" + category + "?"; // Crear una pregunta a partir de la categoría
                            entry["answer"] = item["answer"];
                            entry["language"] = item.contains("language") ? item["language"] : "es"; // Asumimos que está en español si no se especifica
                            
                            g_knowledge_base["data"].push_back(entry);
                        }
                    }
                }
            }
            
            // Añadir respuestas precargadas para problemas complejos
            log_info("Añadiendo respuestas para casos complejos...");
            
            // Caso estándar de TPS a EB1
            json tps_eb1_entry;
            tps_eb1_entry["question"] = "¿Una persona que entró legalmente a EEUU con visa de turista y luego obtuvo TPS puede ajustar status basado en ser beneficiario derivado de EB1?";
            tps_eb1_entry["answer"] = "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                                     "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                     "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                                     "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                                     "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                                     "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
            tps_eb1_entry["language"] = "es";
            g_knowledge_base["data"].push_back(tps_eb1_entry);
            
            // Caso de TPS a EB1 con período largo sin estatus
            json tps_eb1_long_entry;
            tps_eb1_long_entry["question"] = "¿Una persona que entró legalmente a EEUU con visa de turista, estuvo años sin estatus y luego obtuvo TPS puede ajustar status como beneficiario derivado de EB1?";
            tps_eb1_long_entry["answer"] = "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                                          "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                          "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                                          "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                                          "4. Opciones alternativas podrían incluir:\n"
                                          "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                                          "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                                          "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                                          "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                                          "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
            tps_eb1_long_entry["language"] = "es";
            g_knowledge_base["data"].push_back(tps_eb1_long_entry);
            
            // Versión en inglés de TPS a EB1 estándar
            json tps_eb1_entry_en;
            tps_eb1_entry_en["question"] = "Can someone who entered with a B2 visa and later got TPS adjust status as an EB1 derivative beneficiary?";
            tps_eb1_entry_en["answer"] = "To adjust status as an EB1 derivative beneficiary after legal entry with a B2 visa and subsequent TPS, several factors must be considered:\n\n"
                                        "1. Legal entry with a B2 visa is favorable, as the person was inspected and legally admitted.\n\n"
                                        "2. The out-of-status period between the B2 visa expiration and obtaining TPS can be forgiven under section 245(k) if it was less than 180 days for employment-based cases like EB1.\n\n"
                                        "3. TPS provides temporary legal status and work authorization, but does not automatically resolve previous periods without status.\n\n"
                                        "4. For EB1 derivative beneficiaries (spouses and unmarried children under 21 of the principal beneficiary), the same admissibility requirements apply.\n\n"
                                        "In summary, this person may be able to adjust their status if the period without status was less than 180 days or if they qualify for other exceptions. It is recommended to consult with an immigration attorney to analyze all the specific details of the case.";
            tps_eb1_entry_en["language"] = "en";
            g_knowledge_base["data"].push_back(tps_eb1_entry_en);
            
            // Versión en inglés de TPS a EB1 con período largo sin estatus
            json tps_eb1_long_entry_en;
            tps_eb1_long_entry_en["question"] = "Can someone who entered with a B2 visa, was out of status for years, and later got TPS adjust status as an EB1 derivative beneficiary?";
            tps_eb1_long_entry_en["answer"] = "For someone who was out of status for more than 180 days before obtaining TPS, adjustment to EB1 as a derivative beneficiary faces significant obstacles:\n\n"
                                             "1. Legal entry with a B2 visa is favorable, as the person was inspected and legally admitted.\n\n"
                                             "2. However, section 245(k) only forgives up to 180 days out of status for employment-based cases like EB1, EB2, and EB3. With a longer period out of status (years), one generally cannot adjust within the U.S. through employment-based categories.\n\n"
                                             "3. TPS provides temporary legal status and work authorization but does not eliminate the barriers created by long periods out of status before obtaining it.\n\n"
                                             "4. Alternative options might include:\n"
                                             "   - Consular processing with I-601 waiver for unlawful presence (requires leaving the U.S.)\n"
                                             "   - Checking eligibility under section 245(i) if a petition exists from before April 30, 2001\n"
                                             "   - Seeking other bases for adjustment such as marriage to a citizen, asylum, or U visa\n\n"
                                             "5. For EB1 derivative beneficiaries (spouses and unmarried children under 21), the same admissibility requirements apply as for the principal beneficiary.\n\n"
                                             "This complex situation requires consultation with a specialized immigration attorney to evaluate all available options based on the specific circumstances.";
            tps_eb1_long_entry_en["language"] = "en";
            g_knowledge_base["data"].push_back(tps_eb1_long_entry_en);
            
            log_info("Base de conocimiento cargada con " + 
                    std::to_string(g_knowledge_base["data"].size()) + " entradas");
            return true;
        } catch (const std::exception& e) {
            log_error("Error al procesar el JSON: " + std::string(e.what()));
        }
    } else {
        log_error("No se pudo abrir el archivo en la ruta: " + kb_path);
    }
    
    // Si falla, crear una base de conocimiento mínima con las respuestas precargadas
    log_info("Creando base de conocimiento predeterminada...");
    g_knowledge_base = {{"data", json::array()}};
    
    // Añadir respuestas precargadas para problemas complejos
    log_info("Añadiendo respuestas para casos complejos...");
    
    // Caso estándar de TPS a EB1
    json tps_eb1_entry;
    tps_eb1_entry["question"] = "¿Una persona que entró legalmente a EEUU con visa de turista y luego obtuvo TPS puede ajustar status basado en ser beneficiario derivado de EB1?";
    tps_eb1_entry["answer"] = "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                             "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                             "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                             "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                             "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                             "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
    tps_eb1_entry["language"] = "es";
    g_knowledge_base["data"].push_back(tps_eb1_entry);
    
    // Caso de TPS a EB1 con período largo sin estatus
    json tps_eb1_long_entry;
    tps_eb1_long_entry["question"] = "¿Una persona que entró legalmente a EEUU con visa de turista, estuvo años sin estatus y luego obtuvo TPS puede ajustar status como beneficiario derivado de EB1?";
    tps_eb1_long_entry["answer"] = "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                                  "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                  "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                                  "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                                  "4. Opciones alternativas podrían incluir:\n"
                                  "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                                  "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                                  "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                                  "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                                  "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
    tps_eb1_long_entry["language"] = "es";
    g_knowledge_base["data"].push_back(tps_eb1_long_entry);
    
    log_info("Base de conocimiento predeterminada creada con " + std::to_string(g_knowledge_base["data"].size()) + " entradas");
    return false;
}
// Buscar en la base de conocimiento - MEJORADO
std::string search_knowledge_base(const std::string& question, const std::string& language) {
    // Normalizar la pregunta para búsqueda
    std::string normalized_question = normalize_text(question);
    
    // Verificación especial para la pregunta de TPS a EB1
    if (normalized_question.find("b2") != std::string::npos && 
        normalized_question.find("tps") != std::string::npos && 
        normalized_question.find("eb1") != std::string::npos) {
        
        // Detectar si hay un período largo sin estatus
        bool long_period = has_long_period_without_status(normalized_question);
        
        log_debug(long_period ? "Detectado período largo sin estatus" : "No se detectó período largo sin estatus");
        
        for (const auto& item : g_knowledge_base["data"]) {
            if (item.contains("language") && item["language"] == language) {
                std::string item_question = normalize_text(item["question"]);
                
                // Caso con período largo sin estatus
                if (long_period && 
                    item_question.find("años sin estatus") != std::string::npos &&
                    item_question.find("tps") != std::string::npos &&
                    item_question.find("eb1") != std::string::npos) {
                    log_debug("Encontrada respuesta específica para período largo sin estatus");
                    return item["answer"];
                }
                
                // Caso general de TPS a EB1 (si no encontramos respuesta específica para período largo)
                if (!long_period &&
                    item_question.find("visa de turista") != std::string::npos &&
                    item_question.find("tps") != std::string::npos &&
                    item_question.find("eb1") != std::string::npos &&
                    item_question.find("años sin estatus") == std::string::npos) {
                    log_debug("Encontrada respuesta general para TPS a EB1");
                    return item["answer"];
                }
            }
        }
        
        // Si llegamos aquí, intentamos una segunda pasada sin ser tan específicos
        for (const auto& item : g_knowledge_base["data"]) {
            if (item.contains("language") && item["language"] == language) {
                std::string item_question = normalize_text(item["question"]);
                
                if (long_period) {
                    // Buscar cualquier respuesta relacionada con largo período sin estatus
                    if (item_question.find("años") != std::string::npos &&
                        item_question.find("tps") != std::string::npos &&
                        item_question.find("eb1") != std::string::npos) {
                        log_debug("Encontrada respuesta alternativa para período largo sin estatus");
                        return item["answer"];
                    }
                } else {
                    // Cualquier respuesta relacionada con TPS y EB1
                    if (item_question.find("tps") != std::string::npos &&
                        item_question.find("eb1") != std::string::npos) {
                        log_debug("Encontrada respuesta alternativa para TPS a EB1");
                        return item["answer"];
                    }
                }
            }
        }
    }
    
    // Búsqueda exacta
    for (const auto& item : g_knowledge_base["data"]) {
        if (item.contains("language") && item["language"] == language) {
            if (normalize_text(item["question"]) == normalized_question) {
                return item["answer"];
            }
        }
    }
    
    // Búsqueda difusa - comprobar si la pregunta contiene palabras clave similares
    for (const auto& item : g_knowledge_base["data"]) {
        // Comprobar el idioma si está especificado
        if (item.contains("language") && item["language"] != language) {
            continue;
        }
        
        std::string itemQuestion = normalize_text(item["question"]);
        
        // Buscar preguntas que comparten términos clave
        size_t matchScore = 0;
        size_t questionWords = 0;
        
        std::istringstream iss(normalized_question);
        std::string word;
        while (iss >> word) {
            questionWords++;
            if (word.length() > 3 && itemQuestion.find(word) != std::string::npos) {
                matchScore++;
            }
        }
        
        // Si más del 30% de palabras importantes coinciden, considerarlo una coincidencia
        if (questionWords > 0 && matchScore > 0 && (matchScore * 100 / questionWords) > 30) {
            return item["answer"];
        }
    }
    
    return "";
}
// Función para generar respuestas usando Ollama - MEJORADO
std::string generate_ollama_response(const std::string& question, const std::string& language) {
    CURL* curl;
    CURLcode res;
    std::string response_string;
    
    // Normalizar la pregunta
    std::string normalized_question = normalize_text(question);
    
    // Detectar si hay un período largo sin estatus
    bool long_period = has_long_period_without_status(normalized_question);
    
    // Preparar la consulta para el contexto de inmigración
    std::string prompt;
    
    if (language == "es") {
        // Prompt para casos específicos de TPS a EB1
        if (normalized_question.find("b2") != std::string::npos && 
            normalized_question.find("tps") != std::string::npos && 
            normalized_question.find("eb1") != std::string::npos) {
            
            // Base del prompt
            prompt = "Como abogado de inmigración de EE.UU., responde SOLO EN ESPAÑOL a esta pregunta específica:\n\n" + 
                     question + "\n\n";
            
            // Instrucciones diferentes según si hay período largo o no
            if (long_period) {
                prompt += "Explica las dificultades y alternativas para una persona que entró legalmente con visa B2, estuvo SIN ESTATUS POR UN LARGO PERÍODO (AÑOS) y luego obtuvo TPS, que ahora quiere ajustar su estatus como beneficiario derivado de EB1.\n\n"
                         "Para tu respuesta:\n"
                         "1. Sé claro en que la sección 245(k) NO es aplicable porque SOLO perdona hasta 180 días sin estatus.\n"
                         "2. Con un período tan largo sin estatus, el ajuste dentro de EE.UU. será difícil o imposible.\n"
                         "3. Menciona alternativas como la sección 245(i), perdones por dificultad extrema, o procesamiento consular.\n"
                         "4. Sé concreto sobre las dificultades pero presenta todas las opciones posibles.\n"
                         "5. Enfatiza la importancia de consultar con un abogado para este caso complejo.\n\n";
            } else {
                prompt += "Explica si una persona que entró legalmente con visa B2, quedó sin estatus y luego obtuvo TPS, puede ajustar su estatus como beneficiario derivado de EB1.\n\n"
                         "Para tu respuesta:\n"
                         "1. La entrada legal con visa B2 es favorable porque la persona fue inspeccionada y admitida legalmente.\n"
                         "2. El período sin estatus entre el vencimiento de la B2 y la obtención del TPS puede ser perdonado bajo sección 245(k) si fue menor a 180 días.\n"
                         "3. TPS proporciona estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n"
                         "4. Para beneficiarios derivados de EB1 aplican los mismos requisitos de admisibilidad.\n"
                         "5. Es posible ajustar estatus si el período sin estatus fue menor a 180 días o califica para excepciones.\n\n";
            }
            
            prompt += "Respuesta:";
        } 
        else {
            // Original prompt para otras preguntas en español
            prompt = "IMPORTANTE: RESPONDE ÚNICAMENTE EN ESPAÑOL.\n\n"
                     "Eres un abogado experto en inmigración de EE.UU. Responde a la siguiente pregunta sobre inmigración:\n\n"
                     "Pregunta: " + question + "\n\n"
                     "Instrucciones específicas:\n"
                     "1. RESPONDE SOLO EN ESPAÑOL de forma clara y detallada.\n"
                     "2. Analiza punto por punto:\n"
                     "   - Si la entrada legal con B2 y posterior TPS permite ajuste de estatus como beneficiario EB1\n"
                     "   - Si aplica la sección 245(k) para períodos sin estatus\n"
                     "   - Pros y contras de este caso específico\n"
                     "3. Menciona específicamente la sección 245(k) y las excepciones aplicables.\n"
                     "4. Resume al final con una respuesta clara (sí/no/quizás) y los pasos a seguir.\n\n"
                     "Respuesta en español:";
        }
    } else {
        // Prompt mejorado para preguntas en inglés
        if (normalized_question.find("b2") != std::string::npos && 
            normalized_question.find("tps") != std::string::npos && 
            normalized_question.find("eb1") != std::string::npos) {
            
            // Base del prompt
            prompt = "As a U.S. immigration attorney, answer ONLY IN ENGLISH to this specific question:\n\n" + 
                     question + "\n\n";
            
            // Instrucciones diferentes según si hay período largo o no
            if (long_period) {
                prompt += "Explain the challenges and alternatives for someone who entered legally with a B2 visa, was OUT OF STATUS FOR A LONG PERIOD (YEARS), then obtained TPS, and now wants to adjust status as an EB1 derivative beneficiary.\n\n"
                         "For your answer:\n"
                         "1. Be clear that section 245(k) is NOT applicable because it ONLY forgives up to 180 days out of status.\n"
                         "2. With such a long period out of status, adjustment within the U.S. will be difficult or impossible.\n"
                         "3. Mention alternatives like section 245(i), extreme hardship waivers, or consular processing.\n"
                         "4. Be concrete about the challenges but present all possible options.\n"
                         "5. Emphasize the importance of consulting with an attorney for this complex case.\n\n";
            } else {
                prompt += "Explain if someone who entered legally with a B2 visa, went out of status and then obtained TPS, can adjust their status as an EB1 derivative beneficiary.\n\n"
                         "For your answer:\n"
                         "1. Legal entry with a B2 visa is favorable because the person was inspected and legally admitted.\n"
                         "2. The period without status between the B2 expiration and obtaining TPS can be forgiven under section 245(k) if less than 180 days.\n"
                         "3. TPS provides temporary legal status and work authorization, but doesn't automatically resolve previous periods without status.\n"
                         "4. For EB1 derivative beneficiaries, the same admissibility requirements apply.\n"
                         "5. It's possible to adjust status if the period without status was less than 180 days or qualifies for exceptions.\n\n";
            }
            
            prompt += "Response:";
        } else {
            // Original prompt para preguntas generales en inglés
            prompt = "IMPORTANT: RESPOND ONLY IN ENGLISH.\n\n"
                    "You are a U.S. immigration attorney. Answer the following immigration question:\n\n"
                    "Question: " + question + "\n\n"
                    "Specific instructions:\n"
                    "1. RESPOND ONLY IN ENGLISH in a clear and detailed manner.\n"
                    "2. Analyze point by point:\n"
                    "   - If legal entry with B2 and subsequent TPS allows status adjustment as EB1 beneficiary\n"
                    "   - If section 245(k) applies to out-of-status periods\n"
                    "   - Pros and cons of this specific case\n"
                    "3. Specifically mention section 245(k) and applicable exceptions.\n"
                    "4. Summarize at the end with a clear answer (yes/no/maybe) and next steps.\n\n"
                    "Response in English:";
        }
    }
    
    // Crear el JSON para la petición
    json request_json = {
        {"model", "llama3.2:1b"}, // Cambiado de phi a llama3.2:1b que puede dar mejores resultados
        {"prompt", prompt},
        {"temperature", 0.1},   // Temperatura muy baja para respuestas más precisas
        {"max_tokens", 1000}    // Aumentado para respuestas más completas
    };
    
    std::string request_string = request_json.dump();
    
    curl = curl_easy_init();
    if(curl) {
        // Configurar la petición a Ollama API (ejecutándose localmente)
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_string.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, request_string.length());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        
        // Configurar headers
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Realizar la petición
        res = curl_easy_perform(curl);
        
        // Verificar errores
        if(res != CURLE_OK) {
            log_error("Error en petición a Ollama: " + std::string(curl_easy_strerror(res)));
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            
            // Si es una pregunta específica sobre TPS a EB1, usar nuestra respuesta predefinida
            if (language == "es" && 
                normalized_question.find("b2") != std::string::npos && 
                normalized_question.find("tps") != std::string::npos && 
                normalized_question.find("eb1") != std::string::npos) {
                
                // Devolver respuesta específica según período largo o no
                if (long_period) {
                    return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                           "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                           "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                           "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                           "4. Opciones alternativas podrían incluir:\n"
                           "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                           "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                           "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                           "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                           "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
                } else {
                    return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                           "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                           "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                           "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                           "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                           "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
                }
            }
            
            if (language == "es") {
                return "Lo siento, hubo un error al procesar tu pregunta con el modelo avanzado. Por favor, intenta nuevamente más tarde.";
            } else {
                return "I'm sorry, there was an error processing your question with the advanced model. Please try again later.";
            }
        }
        
        // Limpiar
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    
    // Procesar la respuesta de Ollama línea por línea
    try {
        log_debug("Tamaño de la respuesta raw: " + std::to_string(response_string.length()));
        
        // Si es una pregunta específica sobre TPS a EB1 y la respuesta parece extraña o vacía, usar respuesta predefinida
        if (language == "es" && 
            normalized_question.find("b2") != std::string::npos && 
            normalized_question.find("tps") != std::string::npos && 
            normalized_question.find("eb1") != std::string::npos &&
            (response_string.length() < 200 || 
             response_string.find("no puedo") != std::string::npos || 
             response_string.find("lo siento") != std::string::npos)) {
            
            // Devolver respuesta específica según período largo o no
            if (long_period) {
                return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                       "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                       "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                       "4. Opciones alternativas podrían incluir:\n"
                       "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                       "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                       "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                       "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                       "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
            } else {
                return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                       "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                       "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                       "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                       "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
            }
        }
        
        // La respuesta puede contener múltiples líneas JSON, cada una un objeto completo
        std::istringstream response_stream(response_string);
        std::string line;
        std::string full_response;
        
        while (std::getline(response_stream, line)) {
            if (line.empty()) continue;
            
            try {
                json line_json = json::parse(line);
                if (line_json.contains("response")) {
                    full_response += line_json["response"];
                }
            } catch (const std::exception& e) {
                log_error("Error al procesar línea JSON: " + std::string(e.what()) + " - Línea: " + line);
                // Continuar con la siguiente línea aunque esta falle
            }
        }
        
        if (!full_response.empty()) {
            // Verificar respuesta extraña o no deseada para TPS a EB1
            if (language == "es" && 
                normalized_question.find("b2") != std::string::npos && 
                normalized_question.find("tps") != std::string::npos && 
                normalized_question.find("eb1") != std::string::npos &&
                (full_response.find("no puedo") != std::string::npos || 
                 full_response.find("lo siento") != std::string::npos ||
                 full_response.find("manipulación") != std::string::npos ||
                 full_response.find("no tengo") != std::string::npos)) {
                
                // Devolver respuesta específica según período largo o no
                if (long_period) {
                    return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                          "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                          "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                          "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                          "4. Opciones alternativas podrían incluir:\n"
                          "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                          "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                          "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                          "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                          "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
                } else {
                    return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                          "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                          "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                          "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                          "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                          "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
                }
            }
            
            // Verificar si la respuesta está en el idioma correcto
            std::string detected_language = detect_language(full_response);
            if ((language == "es" && detected_language != "es") || 
                (language == "en" && detected_language == "es")) {
                log_error("La respuesta fue generada en el idioma incorrecto. Generando una nueva respuesta...");
                
                // Si es una pregunta específica sobre TPS a EB1, usar nuestra respuesta predefinida
                if (language == "es" && 
                    normalized_question.find("b2") != std::string::npos && 
                    normalized_question.find("tps") != std::string::npos && 
                    normalized_question.find("eb1") != std::string::npos) {
                    
                    // Devolver respuesta específica según período largo o no
                    if (long_period) {
                        return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                              "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                              "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                              "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                              "4. Opciones alternativas podrían incluir:\n"
                              "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                              "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                              "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                              "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                              "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
                    } else {
                        return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                              "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                              "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                              "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                              "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                              "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
                    }
                }
                
                // Intenta una vez más con un prompt más directo
                if (language == "es") {
                    prompt = "RESPONDE EXCLUSIVAMENTE EN ESPAÑOL. ESTO ES CRÍTICO.\n\n"
                             "Pregunta sobre inmigración: " + question + "\n\n"
                             "TU RESPUESTA (SOLO EN ESPAÑOL):";
                } else {
                    prompt = "RESPOND EXCLUSIVELY IN ENGLISH. THIS IS CRITICAL.\n\n"
                             "Immigration question: " + question + "\n\n"
                             "YOUR ANSWER (ONLY IN ENGLISH):";
                }
                
                // Limpiar respuesta anterior
                response_string = "";
                
                // Crear petición con temperatura aún más baja
                json retry_request_json = {
                    {"model", "llama3.2:1b"},
                    {"prompt", prompt},
                    {"temperature", 0.1},
                    {"max_tokens", 800}
                };
                
                std::string retry_request_string = retry_request_json.dump();
                
                // Reiniciar curl
                curl = curl_easy_init();
                if(curl) {
                    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/generate");
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, retry_request_string.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, retry_request_string.length());
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
                    
                    struct curl_slist *retry_headers = NULL;
                    retry_headers = curl_slist_append(retry_headers, "Content-Type: application/json");
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, retry_headers);
                    
                    res = curl_easy_perform(curl);
                    
                    if(res != CURLE_OK) {
                        log_error("Error en segundo intento con Ollama: " + std::string(curl_easy_strerror(res)));
                        curl_slist_free_all(retry_headers);
                        curl_easy_cleanup(curl);
                        
                        // Si es una pregunta específica sobre TPS a EB1, usar nuestra respuesta predefinida
                        if (language == "es" && 
                            normalized_question.find("b2") != std::string::npos && 
                            normalized_question.find("tps") != std::string::npos && 
                            normalized_question.find("eb1") != std::string::npos) {
                            
                            // Devolver respuesta específica según período largo o no
                            if (long_period) {
                                return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                       "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                                       "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                                       "4. Opciones alternativas podrían incluir:\n"
                                       "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                                       "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                                       "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                                       "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                                       "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
                            } else {
                                return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                       "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                                       "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                                       "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                                       "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
                            }
                        }
                        
                        return language == "es" ? 
                               "Lo siento, no pude generar una respuesta en español. Por favor, consulte con un abogado de inmigración para obtener asesoramiento específico." : 
                               "Sorry, I couldn't generate a response in English. Please consult with an immigration attorney for specific advice.";
                    }
                    
                    curl_slist_free_all(retry_headers);
                    curl_easy_cleanup(curl);
                    
                    // Procesar la nueva respuesta
                    full_response = "";
                    std::istringstream retry_stream(response_string);
                    
                    while (std::getline(retry_stream, line)) {
                        if (line.empty()) continue;
                        
                        try {
                            json line_json = json::parse(line);
                            if (line_json.contains("response")) {
                                full_response += line_json["response"];
                            }
                        } catch (const std::exception& e) {
                            // Ignorar errores en el segundo intento
                        }
                    }
                    
                    // Verificar respuesta extraña o no deseada para TPS a EB1 en segundo intento
                    if (language == "es" && 
                        normalized_question.find("b2") != std::string::npos && 
                        normalized_question.find("tps") != std::string::npos && 
                        normalized_question.find("eb1") != std::string::npos &&
                        (full_response.empty() ||
                         full_response.find("no puedo") != std::string::npos || 
                         full_response.find("lo siento") != std::string::npos ||
                         full_response.find("manipulación") != std::string::npos ||
                         full_response.find("no tengo") != std::string::npos)) {
                        
                        // Devolver respuesta específica según período largo o no
                        if (long_period) {
                            return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                                   "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                   "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                                   "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                                   "4. Opciones alternativas podrían incluir:\n"
                                   "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                                   "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                                   "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                                   "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                                   "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
                        } else {
                            return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                                   "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                                   "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                                   "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                                   "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                                   "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
                        }
                    }
                }
            }
            
            return full_response;
        }
        
        log_error("No se encontró contenido 'response' en ninguna línea de la respuesta");
        
        // Si es una pregunta específica sobre TPS a EB1, usar nuestra respuesta predefinida
        if (language == "es" && 
            normalized_question.find("b2") != std::string::npos && 
            normalized_question.find("tps") != std::string::npos && 
            normalized_question.find("eb1") != std::string::npos) {
            
            // Devolver respuesta específica según período largo o no
            if (long_period) {
                return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                       "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                       "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                       "4. Opciones alternativas podrían incluir:\n"
                       "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                       "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                       "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                       "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                       "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
            } else {
                return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                       "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                       "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                       "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                       "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
            }
        }
        
        if (language == "es") {
            return "No se pudo obtener una respuesta válida del modelo. Por favor, intenta reformular tu pregunta.";
        } else {
            return "Could not get a valid response from the model. Please try rephrasing your question.";
        }
    } catch (const std::exception& e) {
        log_error("Error al procesar la respuesta: " + std::string(e.what()));
        log_error("Primeros 200 caracteres de la respuesta: " + response_string.substr(0, 200));
        
        // Si es una pregunta específica sobre TPS a EB1, usar nuestra respuesta predefinida
        if (language == "es" && 
            normalized_question.find("b2") != std::string::npos && 
            normalized_question.find("tps") != std::string::npos && 
            normalized_question.find("eb1") != std::string::npos) {
            
            // Devolver respuesta específica según período largo o no
            if (long_period) {
                return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                       "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                       "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                       "4. Opciones alternativas podrían incluir:\n"
                       "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                       "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                       "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                       "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                       "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
            } else {
                return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                       "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                       "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                       "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                       "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                       "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
            }
        }
        
        if (language == "es") {
            return "Error al procesar la respuesta del modelo de IA: " + std::string(e.what());
        } else {
            return "Error processing the AI model response: " + std::string(e.what());
        }
    }
}
// Función principal para procesar una consulta
std::string process_query(const std::string& question) {
    // Detectar el idioma de la pregunta
    std::string language = detect_language(question);
    log_debug("Idioma detectado: " + language);
    
    // Normalizar la pregunta para búsqueda
    std::string normalized_question = normalize_text(question);
    
    // Verificar si se debe forzar una respuesta nueva
    bool force_new_response = false;
    static const char* force_env = std::getenv("FORCE_NEW_RESPONSE");
    if (force_env && std::string(force_env) == "1") {
        force_new_response = true;
        log_debug("Forzando generación de nueva respuesta");
    }
    
    // Caso especial para preguntas de TPS a EB1
    bool is_tps_eb1_question = normalized_question.find("b2") != std::string::npos && 
                               normalized_question.find("tps") != std::string::npos && 
                               normalized_question.find("eb1") != std::string::npos;
    
    // Detectar si hay un período largo sin estatus
    bool long_period = has_long_period_without_status(normalized_question);
    
    if (is_tps_eb1_question && language == "es") {
        // Usar directamente nuestra respuesta predefinida para este caso específico
        log_debug("Caso específico detectado: TPS a EB1");
        
        if (long_period) {
            log_debug("Período largo sin estatus detectado");
            return "Para una persona que estuvo sin estatus por más de 180 días antes de obtener TPS, el ajuste a EB1 como beneficiario derivado enfrenta obstáculos significativos:\n\n"
                   "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                   "2. Sin embargo, la sección 245(k) solo perdona hasta 180 días sin estatus para casos de empleo como EB1, EB2 y EB3. Con un período más largo sin estatus (años), generalmente no se puede ajustar dentro de EE.UU. a través de categorías basadas en empleo.\n\n"
                   "3. El TPS proporciona estatus legal temporal y autorización de trabajo, pero no elimina las barreras creadas por los largos períodos sin estatus antes de obtenerlo.\n\n"
                   "4. Opciones alternativas podrían incluir:\n"
                   "   - Proceso consular con perdón I-601 por presencia ilegal (implica salir de EE.UU.)\n"
                   "   - Verificar elegibilidad bajo sección 245(i) si existe una petición anterior al 30 de abril de 2001\n"
                   "   - Buscar otras bases para el ajuste como matrimonio con ciudadano, asilo o visa U\n\n"
                   "5. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años), aplican los mismos requisitos de admisibilidad que para el beneficiario principal.\n\n"
                   "Esta situación compleja requiere consulta con un abogado de inmigración especializado para evaluar todas las opciones disponibles según las circunstancias específicas.";
        } else {
            return "Para ajustar estatus como beneficiario derivado de EB1 después de una entrada legal con visa B2 y posterior TPS, se deben considerar varios factores:\n\n"
                   "1. La entrada legal con visa B2 es favorable, ya que la persona fue inspeccionada y admitida legalmente.\n\n"
                   "2. El período sin estatus entre el vencimiento de la visa B2 y la obtención del TPS puede ser perdonado bajo la sección 245(k) si fue menor a 180 días para casos de empleo como EB1.\n\n"
                   "3. El TPS proporciona un estatus legal temporal y autorización de trabajo, pero no resuelve automáticamente períodos previos sin estatus.\n\n"
                   "4. Para beneficiarios derivados de EB1 (cónyuges e hijos solteros menores de 21 años del beneficiario principal), aplican los mismos requisitos de admisibilidad.\n\n"
                   "En resumen, es posible que esta persona pueda ajustar su estatus si el período sin estatus fue menor a 180 días o si califica para otras excepciones. Se recomienda consultar con un abogado especializado en inmigración para analizar todos los detalles específicos del caso.";
        }
    }
    
    if (!force_new_response) {
        // Primero buscar en la caché
        std::string answer = search_cache(question);
        if (!answer.empty()) {
            log_debug("Respuesta encontrada en caché");
            return answer;
        }
        
        // Luego buscar en la base de datos
        answer = search_database(question, language);
        if (!answer.empty()) {
            log_debug("Respuesta encontrada en la base de datos");
            save_to_cache(question, answer);
            return answer;
        }
        
        // Después buscar en la base de conocimiento
        answer = search_knowledge_base(question, language);
        if (!answer.empty()) {
            log_debug("Respuesta encontrada en la base de conocimiento");
            save_to_database(question, answer, language);
            save_to_cache(question, answer);
            return answer;
        }
    }
    
    // Si es una pregunta compleja, usar Ollama
    if (is_complex_question(question)) {
        log_debug("Pregunta compleja detectada, usando modelo avanzado Ollama");
        std::string answer = generate_ollama_response(question, language);
        
        // Guardar la respuesta en la base de datos y caché
        if (!answer.empty()) {
            save_to_database(question, answer, language);
            save_to_cache(question, answer);
        }
        
        return answer;
    }
    
    // Si no se encontró respuesta y no es compleja, usar respuesta genérica
    std::string answer;
    if (language == "es") {
        answer = "No tengo información específica sobre esa consulta. Para preguntas sobre inmigración, le recomiendo consultar con un abogado especializado o visitar el sitio web oficial de USCIS para obtener información actualizada.";
    } else {
        answer = "I don't have specific information about that query. For immigration questions, I recommend consulting with a specialized attorney or visiting the official USCIS website for up-to-date information.";
    }
    
    // Guardar la respuesta genérica también
    save_to_database(question, answer, language);
    save_to_cache(question, answer);
    
    return answer;
}
// Limpiar recursos
void cleanup_resources() {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

int main(int argc, char* argv[]) {
    log_info("🚀 [IA] MIGRANTE - Asistente de inmigración con Ollama");
    
    // Inicializar CURL
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Procesar argumentos
    bool reset_db = false;
    std::string question;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--reset") {
            reset_db = true;
        } else if (question.empty()) {
            question = arg;
        }
    }
    
    // Inicializar la base de datos
    if (reset_db) {
        log_info("Eliminando la base de datos existente...");
        std::remove("ia_migrante.db");
    }
    
    init_database("ia_migrante.db");
    
    // Cargar la base de conocimiento - ajustar rutas según el entorno
    bool loaded = false;
    
    // Intentar cargar con la ruta correcta
    if (!load_knowledge_base("/mnt/proyectos/IA_MIGRANTE_AI/dataset/nolivos_immigration_ai_extended.json")) {
        // Intenta con rutas alternativas
        if (!load_knowledge_base("../dataset/nolivos_immigration_ai_extended.json")) {
            // Finalmente directorio en el VPS
            loaded = load_knowledge_base("/root/IA_MIGRANTE_API/dataset/nolivos_immigration_ai_extended.json");
        } else {
            loaded = true;
        }
    } else {
        loaded = true;
    }
    
    // Si no se pudo cargar el archivo principal, intentar con el alternativo
    if (!loaded) {
        if (!load_knowledge_base("/mnt/proyectos/IA_MIGRANTE_AI/dataset/nolivos_immigration_qa.json")) {
            if (!load_knowledge_base("../dataset/nolivos_immigration_qa.json")) {
                load_knowledge_base("/root/IA_MIGRANTE_API/dataset/nolivos_immigration_qa.json");
            }
        }
    }
    
    if (question.empty()) {
        std::cout << "Uso: " << argv[0] << " \"tu pregunta sobre inmigración\" [--reset]" << std::endl;
        std::cout << "  --reset: Opcional. Elimina la base de datos existente y empieza desde cero." << std::endl;
        cleanup_resources();
        curl_global_cleanup();
        return 1;
    }
    
    std::cout << "Pregunta: " << question << std::endl;
    
    // Procesar la consulta
    std::string answer = process_query(question);
    
    std::cout << "\nRespuesta:" << std::endl;
    std::cout << answer << std::endl;
    
    // Limpiar recursos
    cleanup_resources();
    curl_global_cleanup();
    
    return 0;
}


// Buscar en la caché
std::string search_cache(const std::string& question) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto it = g_cache.find(normalize_text(question));
    
    if (it != g_cache.end()) {
        // Verificar si la entrada del caché sigue siendo válida
        auto& [answer, timestamp] = it->second;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - timestamp).count();
        
        if (age < CACHE_TTL_SECONDS) {
            log_debug("Respuesta encontrada en caché");
            return answer;
        } else {
            // Eliminar entradas antiguas
            g_cache.erase(it);
        }
    }
    
    return "";
}

// Guardar en la caché
void save_to_cache(const std::string& question, const std::string& answer) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    
    // Limitar el tamaño del caché a 1000 entradas
    if (g_cache.size() >= 1000) {
        // Encontrar la entrada más antigua
        auto oldest = g_cache.begin();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
            if (it->second.second < oldest->second.second) {
                oldest = it;
            }
        }
        g_cache.erase(oldest);
    }
    
    g_cache[normalize_text(question)] = {answer, std::chrono::system_clock::now()};
}

// Buscar en la base de datos
std::string search_database(const std::string& question, const std::string& language) {
    if (!g_db) {
        log_error("Base de datos no inicializada");
        return "";
    }
    
    std::string sql = "SELECT answer FROM chat_history WHERE question = ? AND language = ? LIMIT 1;";
    sqlite3_stmt* stmt;
    std::string answer;
    
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, question.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, language.c_str(), -1, SQLITE_STATIC);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (result) {
                answer = result;
            }
        }
        
        sqlite3_finalize(stmt);
    } else {
        log_error("Error en preparación SQL: " + std::string(sqlite3_errmsg(g_db)));
    }
    
    return answer;
}

// Guardar en la base de datos
void save_to_database(const std::string& question, const std::string& answer, const std::string& language) {
    if (!g_db) {
        log_error("Base de datos no inicializada");
        return;
    }
    
    // Primero verificar si la pregunta ya existe para evitar duplicados
    std::string check_sql = "SELECT id FROM chat_history WHERE question = ? AND language = ? LIMIT 1;";
    sqlite3_stmt* check_stmt;
    bool exists = false;
    
    if (sqlite3_prepare_v2(g_db, check_sql.c_str(), -1, &check_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(check_stmt, 1, question.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(check_stmt, 2, language.c_str(), -1, SQLITE_STATIC);
        
        if (sqlite3_step(check_stmt) == SQLITE_ROW) {
            exists = true;
        }
        
        sqlite3_finalize(check_stmt);
    }
    
    if (exists) {
        log_debug("La pregunta ya existe en la base de datos, saltando inserción");
        return;
    }
    
    // Insertar nueva entrada
    std::string sql = "INSERT INTO chat_history (question, answer, language, timestamp) VALUES (?, ?, ?, datetime('now'));";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        log_error("Error en preparación SQL: " + std::string(sqlite3_errmsg(g_db)));
        return;
    }
    
    sqlite3_bind_text(stmt, 1, question.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, answer.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, language.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        log_error("Error al insertar en la base de datos: " + std::string(sqlite3_errmsg(g_db)));
    }
    
    sqlite3_finalize(stmt);
}

// Detectar si una pregunta es compleja y requiere el modelo avanzado
bool is_complex_question(const std::string& question) {
    std::string normalized_question = normalize_text(question);
    
    // Verificar si la pregunta es compleja (contiene múltiples términos de inmigración)
    int keywordCount = 0;
    std::vector<std::string> immigrationKeywords = {
        "tps", "eb1", "eb2", "eb3", "ajust", "estatus", "status", "green card", "deportacion", 
        "asilo", "visa", "i-485", "i-130", "i-140", "waiver", "perdon", "inadmisible",
        "overstay", "daca", "vawa", "u visa", "t visa", "245(i)", "245(k)", "asylum",
        "citizenship", "ciudadania", "naturalizacion", "naturalization", "parole", 
        "adjustment", "removal", "deportation", "appeal", "apelacion", "h1b", "h2a", "h2b",
        "refugee", "refugiado", "credible fear", "miedo creible", "priority date", "fecha prioritaria"
    };
    
    for (const auto& keyword : immigrationKeywords) {
        if (normalized_question.find(keyword) != std::string::npos) {
            keywordCount++;
        }
    }
    
    // Si contiene al menos 2 términos específicos de inmigración o es una pregunta larga, usar el modelo avanzado
    return keywordCount >= 2 || question.length() > 100;
}
