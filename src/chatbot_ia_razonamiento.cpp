#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstring>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include "Crow/include/crow.h"
#include "llama.cpp/include/llama.h"

using json = nlohmann::json;

// Global variables
sqlite3* g_db = nullptr;
json g_knowledge_base;
std::mutex g_mutex;

// Helper functions
void log_info(const std::string& message) {
    std::cout << "✅ [INFO] " << message << std::endl;
}

void log_error(const std::string& message) {
    std::cout << "❌ [ERROR] " << message << std::endl;
}

void log_debug(const std::string& message) {
    std::cout << "🔍 [DEBUG] " << message << std::endl;
}

// Initialize database with corrected schema
bool init_database(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &g_db) != SQLITE_OK) {
        log_error("Failed to open database: " + std::string(sqlite3_errmsg(g_db)));
        return false;
    }
    
    // Comprobar si la tabla chat_history ya existe
    sqlite3_stmt* check_stmt;
    bool table_exists = false;
    
    if (sqlite3_prepare_v2(g_db, "SELECT name FROM sqlite_master WHERE type='table' AND name='chat_history';", -1, &check_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(check_stmt) == SQLITE_ROW) {
            table_exists = true;
        }
        sqlite3_finalize(check_stmt);
    }
    
    if (table_exists) {
        // La tabla ya existe, verificar si tiene la columna timestamp
        sqlite3_stmt* col_stmt;
        bool has_timestamp = false;
        
        if (sqlite3_prepare_v2(g_db, "PRAGMA table_info(chat_history);", -1, &col_stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(col_stmt) == SQLITE_ROW) {
                const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(col_stmt, 1));
                if (col_name && strcmp(col_name, "timestamp") == 0) {
                    has_timestamp = true;
                    break;
                }
            }
            sqlite3_finalize(col_stmt);
            
            if (!has_timestamp) {
                // Añadir la columna timestamp
                char* errMsg = nullptr;
                if (sqlite3_exec(g_db, "ALTER TABLE chat_history ADD COLUMN timestamp DATETIME DEFAULT CURRENT_TIMESTAMP;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
                    std::string error = errMsg;
                    sqlite3_free(errMsg);
                    log_error("Error al añadir columna timestamp: " + error);
                } else {
                    log_info("Columna timestamp añadida correctamente");
                }
            }
        }
    } else {
        // La tabla no existe, crearla desde cero
        const char* sql = 
            "CREATE TABLE IF NOT EXISTS chat_history ("
            "  id INTEGER PRIMARY KEY, "
            "  question TEXT NOT NULL, "
            "  answer TEXT NOT NULL, "
            "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_question ON chat_history(question);";
        
        char* errMsg = nullptr;
        if (sqlite3_exec(g_db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::string error = errMsg;
            sqlite3_free(errMsg);
            log_error("Failed to create tables: " + error);
            return false;
        }
    }
    
    log_info("Base de datos inicializada correctamente");
    return true;
}

// Load knowledge base with better error handling and fallback to local file
bool load_knowledge_base(const std::string& kb_path) {
    // Intentar cargar desde la ruta proporcionada
    std::ifstream file(kb_path);
    
    if (file.is_open()) {
        try {
            file >> g_knowledge_base;
            log_info("Base de conocimiento cargada con " + 
                    std::to_string(g_knowledge_base["data"].size()) + " entradas");
            return true;
        } catch (const std::exception& e) {
            log_error("Error al procesar el JSON: " + std::string(e.what()));
        }
    } else {
        log_error("No se pudo abrir el archivo en la ruta: " + kb_path);
    }
    
    // Si falla, intentar con el archivo alternativo
    std::string alt_path = "/mnt/proyectos/IA_MIGRANTE_AI/dataset/nolivos_immigration_qa.json";
    std::ifstream alt_file(alt_path);
    
    if (alt_file.is_open()) {
        try {
            alt_file >> g_knowledge_base;
            log_info("Base de conocimiento alternativa cargada con " + 
                    std::to_string(g_knowledge_base["data"].size()) + " entradas");
            return true;
        } catch (const std::exception& e) {
            log_error("Error al procesar el JSON alternativo: " + std::string(e.what()));
        }
    }
    
    // Si todos los intentos fallan, crear una base de conocimiento mínima
    log_info("Creando base de conocimiento predeterminada...");
    g_knowledge_base = {{"data", json::array()}};
    
    // Añadir algunos ejemplos
    g_knowledge_base["data"].push_back({
        {"question", "¿Qué es una visa de trabajo?"},
        {"answer", "Una visa de trabajo es un documento oficial que permite a un extranjero trabajar legalmente en un país durante un período determinado. Los requisitos y procesos varían según el país emisor y el tipo de trabajo."}
    });
    
    g_knowledge_base["data"].push_back({
        {"question", "¿Cómo solicitar asilo?"},
        {"answer", "El proceso de solicitud de asilo generalmente implica presentarse ante las autoridades migratorias y expresar temor de regresar al país de origen debido a persecución por motivos de raza, religión, nacionalidad, opinión política o pertenencia a un grupo social específico. Es recomendable buscar asesoría legal especializada."}
    });
    
    log_info("Base de conocimiento predeterminada creada con 2 entradas");
    return true;
}

// Search database for an answer - FIXED SQL query
std::string search_database(const std::string& question) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    std::string sql = "SELECT answer FROM chat_history WHERE question = ? LIMIT 1;";
    sqlite3_stmt* stmt;
    std::string answer;
    
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, question.c_str(), -1, SQLITE_STATIC);
        
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

// Search knowledge base for an answer with improved matching
std::string search_knowledge_base(const std::string& question) {
    // Convert to lowercase for case-insensitive comparison
    std::string lowercaseQuestion = question;
    std::transform(lowercaseQuestion.begin(), lowercaseQuestion.end(), lowercaseQuestion.begin(), 
                   [](unsigned char c){ return std::tolower(c); });
    
    // Exact match search
    for (const auto& item : g_knowledge_base["data"]) {
        if (item["question"] == question) {
            return item["answer"];
        }
    }
    
    // Fuzzy search - check if question contains similar keywords
    for (const auto& item : g_knowledge_base["data"]) {
        std::string itemQuestion = item["question"];
        std::transform(itemQuestion.begin(), itemQuestion.end(), itemQuestion.begin(), 
                       [](unsigned char c){ return std::tolower(c); });
        
        // Look for questions that share key terms
        size_t matchScore = 0;
        size_t questionWords = 0;
        
        std::istringstream iss(lowercaseQuestion);
        std::string word;
        while (iss >> word) {
            questionWords++;
            if (word.length() > 3 && itemQuestion.find(word) != std::string::npos) {
                matchScore++;
            }
        }
        
        // If more than 50% of important words match, consider it a match
        if (questionWords > 0 && matchScore > 0 && (matchScore * 100 / questionWords) > 50) {
            return item["answer"];
        }
    }
    
    return "";
}

// Save conversation to database - FIXED SQL query and error handling
void save_to_database(const std::string& question, const std::string& answer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Primero verificar si la pregunta ya existe para evitar duplicados
    std::string check_sql = "SELECT id FROM chat_history WHERE question = ? LIMIT 1;";
    sqlite3_stmt* check_stmt;
    bool exists = false;
    
    if (sqlite3_prepare_v2(g_db, check_sql.c_str(), -1, &check_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(check_stmt, 1, question.c_str(), -1, SQLITE_STATIC);
        
        if (sqlite3_step(check_stmt) == SQLITE_ROW) {
            exists = true;
        }
        
        sqlite3_finalize(check_stmt);
    }
    
    if (exists) {
        log_debug("La pregunta ya existe en la base de datos, saltando inserción");
        return;
    }
    
    // Intentar insertar con timestamp
    std::string sql = "INSERT INTO chat_history (question, answer, timestamp) VALUES (?, ?, datetime('now'));";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        // Si falla, intentar sin timestamp (compatibilidad con tabla antigua)
        log_debug("Intentando inserción sin timestamp");
        sql = "INSERT INTO chat_history (question, answer) VALUES (?, ?);";
        
        if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            log_error("Error en preparación SQL: " + std::string(sqlite3_errmsg(g_db)));
            return;
        }
    }
    
    sqlite3_bind_text(stmt, 1, question.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, answer.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        log_error("Error al insertar en la base de datos: " + std::string(sqlite3_errmsg(g_db)));
    }
    
    sqlite3_finalize(stmt);
}

// Generate a response based on the question - IMPROVED with more keywords
std::string generate_response(const std::string& question) {
    // Respuestas predefinidas basadas en palabras clave - versión ampliada
    const std::vector<std::pair<std::string, std::string>> responses = {
        // Visas - General
        {"visa", "Para obtener información sobre visas, debe consultar el sitio web oficial de la embajada o consulado del país al que desea viajar. Cada país tiene requisitos específicos para diferentes tipos de visas (turismo, trabajo, estudio, etc.). Es importante presentar una solicitud completa con toda la documentación requerida y con suficiente antelación al viaje planeado."},
        
        // Visas de trabajo
        {"trabajo", "Las visas de trabajo generalmente requieren una oferta de empleo de un empleador en el país de destino. El empleador puede necesitar demostrar que no hay ciudadanos o residentes cualificados para el puesto. Dependiendo del país, puede haber diferentes categorías de visas de trabajo, como para trabajadores altamente cualificados, temporales o estacionales. El proceso suele incluir verificaciones de antecedentes y, en algunos casos, exámenes médicos."},
        {"h1b", "La visa H-1B es un visado temporal de trabajo para EE.UU. destinado a profesionales en ocupaciones especializadas. Requiere un patrocinador empleador, título universitario relevante o experiencia equivalente, y está sujeta a un límite anual. Si desea cambiar de empleador, generalmente necesitará que el nuevo empleador presente una nueva petición H-1B antes de cambiar de trabajo."},
        {"h2a", "La visa H-2A permite a trabajadores agrícolas extranjeros trabajar temporalmente en EE.UU. Los empleadores deben demostrar que no hay suficientes trabajadores estadounidenses disponibles y que la contratación no afectará negativamente los salarios locales. Incluye requisitos como vivienda, transporte y garantía de empleo por al menos 75% del período contratado."},
        {"h2b", "La visa H-2B permite a empleadores estadounidenses contratar trabajadores extranjeros para empleos temporales no agrícolas. Está sujeta a un límite anual y requiere demostrar que no hay trabajadores estadounidenses disponibles. Los trabajos deben ser de naturaleza temporal (necesidad única, estacional, demanda pico o intermitente)."},
        {"l1", "La visa L-1 permite a empresas multinacionales transferir ejecutivos, gerentes o empleados con conocimientos especializados a sus oficinas en EE.UU. La L-1A (para ejecutivos/gerentes) puede durar hasta 7 años, mientras que la L-1B (conocimiento especializado) hasta 5 años. Requiere que el solicitante haya trabajado para la empresa en el extranjero por al menos 1 año en los últimos 3 años."},
        {"o1", "La visa O-1 está destinada a personas con habilidades extraordinarias en ciencias, artes, educación, negocios o deportes. Requiere demostrar reconocimiento nacional o internacional en su campo a través de premios, publicaciones, contribuciones significativas u otros criterios específicos. No tiene límite anual y puede permitir estadías de hasta 3 años con posibles extensiones."},
        {"permiso trabajo", "Los permisos de trabajo son documentos que autorizan legalmente a extranjeros a trabajar en un país. Los requisitos y procesos para obtenerlos varían significativamente según el país. Generalmente, se necesita una oferta de trabajo válida, documentación personal, y en algunos casos, demostrar calificaciones específicas. La duración y condiciones del permiso dependen del tipo de programa migratorio y las políticas del país."},
        
        // Visas de estudiante
        {"estudiante", "Las visas de estudiante requieren generalmente una carta de aceptación de una institución educativa reconocida, prueba de fondos suficientes para mantenerse durante los estudios, y a veces un seguro médico. Muchos países permiten a los estudiantes trabajar parcialmente durante sus estudios y ofrecen períodos posteriores para buscar empleo. Es importante mantener un estatus académico completo para conservar la validez de la visa."},
        {"f1", "La visa F-1 es para estudiantes académicos en EE.UU. Requiere aceptación en un programa a tiempo completo, prueba de capacidad financiera y vínculos con el país de origen. Permite trabajo en campus y, después del primer año, posibles prácticas profesionales (CPT/OPT). Tras graduarse, es posible solicitar OPT por 12 meses (extendible a 36 meses para campos STEM)."},
        {"j1", "La visa J-1 es para participantes en programas de intercambio en EE.UU., incluyendo estudiantes, investigadores, profesores, au pairs y médicos. Muchos programas J-1 tienen un requisito de residencia de dos años en el país de origen tras completar el programa. Permite empleo relacionado con el programa de intercambio con aprobación previa del patrocinador."},
        
        // Residencia permanente
        {"residencia permanente", "La residencia permanente otorga el derecho a vivir y trabajar indefinidamente en un país. Los caminos para obtenerla incluyen patrocinio familiar, empleo, inversión, asilo o programas especiales. Los requisitos generalmente incluyen buen carácter moral, ausencia de antecedentes penales graves, y a veces, conocimiento del idioma y cultura local. El proceso puede tomar desde meses hasta varios años dependiendo del país y la categoría."},
        {"green card", "La Green Card (Tarjeta de Residente Permanente) otorga residencia permanente legal en EE.UU. Puede obtenerse a través de familia, empleo, la lotería de visas, asilo o programas especiales. El proceso generalmente incluye una petición, solicitud de ajuste de estatus o proceso consular, revisión de antecedentes y entrevista. Los titulares pueden vivir y trabajar permanentemente en EE.UU. y solicitar la ciudadanía después de 3-5 años."},
        {"express entry", "Express Entry es el sistema de inmigración de Canadá para trabajadores cualificados. Gestiona solicitudes para programas federales como el Programa de Trabajadores Calificados, Oficios Especializados y Experiencia Canadiense. Los candidatos reciben puntuaciones basadas en edad, educación, experiencia laboral e idioma, y los de mayor puntuación reciben invitaciones para solicitar residencia permanente."},
        {"arraigo", "El arraigo es un proceso en España que permite a extranjeros en situación irregular obtener residencia legal si demuestran ciertos vínculos con el país. Hay tres tipos: laboral (2+ años en España, 6+ meses trabajando), social (3+ años en España, contrato laboral, vínculos familiares o informe de integración) y familiar (ser padre de español o hijo de originalmente español). Cada tipo tiene requisitos específicos de documentación."},
        
        // Asilo y refugio
        {"asilo", "El asilo se otorga a personas que tienen un temor fundado de persecución en su país de origen por motivos de raza, religión, nacionalidad, opinión política o pertenencia a un grupo social particular. El proceso generalmente implica una solicitud formal, entrevistas, y evaluación de evidencias. Durante el trámite, muchos países proporcionan autorización de trabajo temporal. Es importante buscar asesoramiento legal para el proceso de solicitud."},
        {"refugiado", "El estatus de refugiado se otorga a personas que han huido de su país debido a persecución, guerra o violencia. A diferencia del asilo (solicitado dentro del país de destino), el estatus de refugiado suele solicitarse desde fuera del país donde se busca protección, a menudo a través de ACNUR. Los refugiados reconocidos reciben protección legal, asistencia para necesidades básicas, y eventualmente, posibilidades de integración o reasentamiento."},
        {"protección temporal", "La Protección Temporal es un estatus que brinda refugio a corto plazo a personas desplazadas por conflictos, violencia o desastres. El Estatus de Protección Temporal (TPS) en EE.UU. se designa para países específicos enfrentando condiciones extraordinarias, permitiendo a sus nacionales permanecer y trabajar legalmente por períodos definidos. Las designaciones actuales incluyen países como Venezuela, Haití, Somalia, Sudán, entre otros, y se renuevan periódicamente."},
        {"tps", "El Estatus de Protección Temporal (TPS) es un programa de EE.UU. que permite a nacionales de países designados permanecer temporalmente debido a conflictos, desastres naturales u otras condiciones extraordinarias. Proporciona protección contra la deportación y autorización de trabajo. Las designaciones son temporales pero pueden renovarse. Actualmente incluye países como Venezuela, Haití, El Salvador, Honduras, Nepal, Nicaragua, Somalia, Sudán, Sudán del Sur, Siria y Yemen, aunque esto puede cambiar."},
        
        // Reunificación familiar
        {"familia", "La reunificación familiar permite a ciertos residentes legales y ciudadanos patrocinar a familiares para inmigrar. Los familiares elegibles generalmente incluyen cónyuges, hijos, padres y, en algunos casos, hermanos. El patrocinador debe demostrar capacidad financiera para mantener a los familiares. Los tiempos de procesamiento varían significativamente según el país, la relación familiar y las cuotas anuales. En muchos casos, existe un sistema de preferencias con tiempos de espera diferentes."},
        {"cónyuge", "Las visas o permisos para cónyuges permiten la reunificación de parejas legalmente casadas. El patrocinador debe ser ciudadano o residente legal y generalmente debe demostrar que el matrimonio es genuino y no con fines migratorios. En muchos países, este proceso incluye entrevistas, evidencia de la relación y, en algunos casos, requisitos de ingresos mínimos. Algunos países también reconocen uniones civiles o parejas de hecho para la inmigración."},
        {"matrimonio", "La inmigración basada en matrimonio permite a ciudadanos o residentes permanentes patrocinar a sus cónyuges extranjeros. El proceso suele incluir una petición inicial, evidencia de matrimonio genuino (fotos, comunicaciones, testimonio de testigos), documentación personal, revisión de antecedentes, examen médico y una entrevista. Las autoridades evalúan cuidadosamente que no sea un matrimonio fraudulento. En algunos países, se emite primero una residencia condicional por 2 años."},
        {"padres", "La inmigración de padres varía según el país. En EE.UU., ciudadanos mayores de 21 años pueden patrocinar a sus padres como familiares inmediatos, sin límites numéricos. En Canadá, existe el Programa de Padres y Abuelos con cupos limitados. España permite reunificación tras un año de residencia legal. Australia ofrece visas de padres con opciones contributivas y no contributivas. Todos requieren demostrar capacidad financiera para mantener a los padres patrocinados."},
        {"hijos", "La inmigración de hijos generalmente tiene prioridad en sistemas de reunificación familiar. Para hijos menores, el proceso suele ser más rápido y directo. Para hijos adultos, muchos países tienen restricciones de edad y pueden requerir demostrar dependencia económica. Documentos importantes incluyen certificados de nacimiento, prueba de custodia legal (en caso de padres divorciados), y a veces pruebas de ADN si la documentación es insuficiente."},
        
        // Ciudadanía y naturalización
        {"ciudadanía", "Los requisitos para la ciudadanía generalmente incluyen un período de residencia legal (típicamente 3-5 años), conocimiento del idioma y de la historia/gobierno del país, buen carácter moral (sin antecedentes penales significativos), y aprobar un examen de ciudadanía. El proceso incluye solicitud, biométricos, entrevista y ceremonia de juramento. Muchos países permiten la doble ciudadanía, pero no todos, por lo que es importante verificar si renunciar a la ciudadanía original es necesario."},
        {"naturalización", "La naturalización es el proceso legal por el cual un extranjero adquiere la ciudadanía. Los requisitos típicos incluyen: residencia legal por un período específico (generalmente 3-7 años), conocimiento del idioma, historia y sistema político, buen carácter moral, y juramento de lealtad. Se requiere presentar documentación completa, pagar tarifas, asistir a una entrevista y, en la mayoría de los casos, aprobar un examen. Tras la aprobación, se participa en una ceremonia de ciudadanía."},
        {"doble nacionalidad", "La doble nacionalidad permite a una persona ser ciudadana de dos países simultáneamente. No todos los países la permiten; algunos exigen renunciar a la ciudadanía anterior al naturalizarse, mientras que otros la aceptan plenamente. Países como EE.UU., Canadá, Reino Unido, Australia, México y la mayoría de países de la UE aceptan la doble nacionalidad. Es importante verificar las leyes específicas tanto del país de origen como del país de naturalización para evitar perder derechos o incurrir en obligaciones inesperadas."},
        
        // Deportación y problemas legales
        {"deportación", "Si enfrenta una posible deportación, busque asesoramiento legal inmediatamente. Puede tener opciones para permanecer legalmente dependiendo de su situación particular, como asilo, cancelación de remoción, ajuste de estatus o salida voluntaria. Un abogado de inmigración puede ayudarle a entender sus derechos y defensas legales. No ignore avisos de comparecencia ante el tribunal de inmigración, ya que podría resultar en una orden de deportación en ausencia."},
        {"remoción", "La remoción (deportación) puede ser impugnada a través de varias opciones legales. La Cancelación de Remoción requiere residencia continua (7-10 años dependiendo del estatus), buen carácter moral y demostrar dificultad excepcional para familiares ciudadanos/residentes si ocurre la deportación. Otras defensas incluyen asilo, protección bajo la Convención Contra la Tortura, visas U/T para víctimas de crímenes/tráfico, y ajuste de estatus si es elegible. Es crucial obtener representación legal especializada."},
        {"orden de deportación", "Si ha recibido una orden de deportación, tiene opciones como: 1) Apelación a la Junta de Apelaciones de Inmigración (dentro de 30 días), 2) Moción para reabrir o reconsiderar el caso, 3) Solicitud de suspensión de deportación, 4) Protección bajo la Convención Contra la Tortura, o 5) Salida voluntaria para evitar las consecuencias de una deportación formal. Dependiendo de las circunstancias, también podría ser elegible para alivios humanitarios. Consulte inmediatamente a un abogado de inmigración."},
        {"antecedentes penales", "Los antecedentes penales pueden afectar significativamente el estatus migratorio. Delitos considerados como 'agravados' o de 'bajeza moral' pueden resultar en deportación incluso para residentes permanentes. Infracciones como DUI pueden afectar solicitudes de ciudadanía o visas. Es crucial divulgar honestamente cualquier antecedente en solicitudes migratorias y consultar con un abogado especializado antes de declararse culpable de cualquier delito, ya que las consecuencias migratorias pueden ser más severas que las penales."},
        {"dui", "Un DUI (conducción bajo influencia) puede tener serias consecuencias migratorias. Para solicitudes de naturalización, un DUI reciente (5 años o menos) puede demostrar falta de 'buen carácter moral'. Múltiples DUIs o casos agravados pueden llevar a denegación de visas, inadmisibilidad al país o incluso deportación. Aunque un solo DUI sin agravantes generalmente no causa deportación para residentes permanentes, puede complicar futuros trámites migratorios y viajes internacionales. Se recomienda encarecidamente consultar con un abogado de inmigración especializado."},
        
        // Programas especiales
        {"daca", "DACA (Acción Diferida para los Llegados en la Infancia) ofrece protección temporal contra la deportación y autorización de trabajo para ciertas personas traídas a EE.UU. como niños. Los requisitos incluyen llegada antes de los 16 años, residencia continua desde 2007, educación (graduado/GED/actualmente en escuela), y no tener condenas por delitos graves. DACA se otorga por dos años y puede renovarse. No proporciona un camino directo a la residencia permanente o ciudadanía, pero permite solicitar advance parole para viajar."},
        {"vawa", "VAWA (Ley de Violencia Contra las Mujeres) permite a víctimas de abuso doméstico por parte de ciudadanos o residentes permanentes de EE.UU. solicitar residencia por cuenta propia, sin depender del abusador. Tanto mujeres como hombres pueden solicitarla si demuestran que sufrieron abuso físico o extrema crueldad, que el matrimonio era de buena fe, y que tienen buen carácter moral. VAWA ofrece confidencialidad, protegiendo a las víctimas de la notificación a sus abusadores sobre su solicitud."},
        {"visa u", "La Visa U es para víctimas de ciertos delitos (incluyendo violencia doméstica, agresión sexual, tráfico humano) que han sufrido abuso mental o físico y ayudan a las autoridades en la investigación o procesamiento del delito. Requiere certificación de una agencia de aplicación de la ley y permite residencia temporal por 4 años, autorización de trabajo, y la posibilidad de solicitar residencia permanente después de 3 años. También pueden incluirse ciertos familiares en la solicitud."},
        {"visa t", "La Visa T es para víctimas de tráfico humano (sexual o laboral) que están en EE.UU. debido al tráfico, cooperan con las autoridades (salvo menores o excepciones por trauma), y demuestran que sufrirían dificultades extremas si fueran deportadas. Proporciona residencia temporal por 4 años, autorización de trabajo, beneficios públicos y la posibilidad de solicitar residencia permanente después de 3 años. Ciertos familiares cercanos también pueden recibir estatus derivado."},
        
        // Estatus y cambios
{"renovar", "Para renovar su estatus migratorio, generalmente debe presentar una solicitud antes de que expire su estatus actual. Comience el proceso con al menos 3-6 meses de antelación. Verifique que siga cumpliendo los requisitos de elegibilidad, prepare documentación actualizada (pasaporte, evidencia de mantenimiento de estatus), y pague las tarifas correspondientes. En muchos casos, puede permanecer legalmente mientras su solicitud de renovación está pendiente, si la presentó antes del vencimiento."},
{"cambio de estatus", "El cambio de estatus permite modificar la categoría migratoria sin salir del país. No todos los cambios son permitidos (como de turista a residente permanente directamente). Requiere estar en estatus legal al solicitar, tener visa válida para la nueva categoría, y cumplir requisitos específicos. Algunas restricciones pueden aplicar, especialmente si entró con visa de no inmigrante pero tenía intención de quedarse. El proceso incluye formularios específicos, documentación de respaldo y, a veces, entrevistas."},
{"ajuste de estatus", "El ajuste de estatus es el proceso para obtener residencia permanente (Green Card) mientras está dentro de EE.UU., evitando el procesamiento consular en el extranjero. Es necesario ser elegible para una Green Card por familia, empleo u otra categoría, haber sido inspeccionado y admitido legalmente (con algunas excepciones), y mantener estatus legal (con excepciones para familiares inmediatos de ciudadanos). El proceso incluye formularios, examen médico, biométricos, y posiblemente una entrevista."},
{"caducada", "Si su visa o estatus ha caducado, las consecuencias y opciones varían según el país y su situación. En muchos casos, permanecer después del vencimiento puede resultar en prohibiciones de reingreso, dificultades para futuras solicitudes de visa, o deportación. Opciones potenciales incluyen: solicitar prórroga (si aún está dentro del período permitido), cambio de estatus, ajuste a residencia permanente si es elegible, salida voluntaria, o en algunos casos, solicitar alivio por razones humanitarias o dificultades extremas."},
{"overstay", "Permanecer más allá del período autorizado (overstay) puede tener graves consecuencias migratorias. En EE.UU., overstays de más de 180 días conllevan prohibición de reingreso de 3 años; más de 1 año resulta en prohibición de 10 años. Afecta futuros trámites migratorios y puede llevar a deportación. Algunas opciones incluyen: matrimonio con ciudadano (si es genuino), asilo (si califica), visas U/T para víctimas de crímenes, o perdones por dificultad extrema para familiares ciudadanos/residentes. Consulte urgentemente a un abogado de inmigración."},

// Consulta legal
{"abogado", "Para asuntos migratorios, es altamente recomendable consultar con un abogado especializado en inmigración o representante acreditado. Pueden evaluar su caso específico, explicar opciones migratorias, preparar y presentar solicitudes, representarle ante autoridades migratorias y tribunales, y ayudarle a navegar procesos complejos. Para encontrar representación legal asequible, considere organizaciones sin fines de lucro de servicios legales, clínicas legales universitarias, o programas pro bono en su área."}
};

// Convertir pregunta a minúsculas para búsqueda insensible a mayúsculas/minúsculas
std::string lowercaseQuestion = question;
std::transform(lowercaseQuestion.begin(), lowercaseQuestion.end(), lowercaseQuestion.begin(), 
               [](unsigned char c){ return std::tolower(c); });

// Buscar palabras clave en la pregunta
for (const auto& response : responses) {
    if (lowercaseQuestion.find(response.first) != std::string::npos) {
        return response.second;
    }
}

// Respuesta predeterminada si no se encontraron palabras clave
return "Soy IA MIGRANTE, un asistente virtual para temas de inmigración. Puedo proporcionar información general sobre visas, asilo, permisos de trabajo, reunificación familiar y otros temas relacionados con inmigración. Para obtener asesoramiento legal específico sobre su caso, le recomendamos consultar con un abogado de inmigración calificado.";
}

// Process query through all sources
std::string process_query(const std::string& question) {
    // First check database cache
    std::string answer = search_database(question);
    if (!answer.empty()) {
        log_debug("Respuesta encontrada en la base de datos");
        return answer;
    }
    
    // Then check knowledge base
    answer = search_knowledge_base(question);
    if (!answer.empty()) {
        log_debug("Respuesta encontrada en la base de conocimiento");
        save_to_database(question, answer);
        return answer;
    }
    
    // Generate response based on keywords
    log_debug("Generando respuesta basada en palabras clave");
    answer = generate_response(question);
    save_to_database(question, answer);
    
    return answer;
}

// Clean up resources
void cleanup_resources() {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

// Main function
int main() {
    log_info("🚀 [IA] MIGRANTE - Iniciando API de inmigración (versión mejorada)...");
    
    // Initialize components
    if (!init_database("chatbot_data.db")) {
        log_error("Error al inicializar la base de datos");
        return 1;
    }
    
    // Usar la ruta exacta a tu dataset
    if (!load_knowledge_base("/mnt/proyectos/IA_MIGRANTE_AI/dataset/nolivos_immigration_ai_extended.json")) {
        log_error("No se pudo cargar la base de conocimiento principal (usando fuente alternativa)");
    }
    
    // Set up Crow app
    crow::SimpleApp app;
    
    // Chatbot API endpoint
    CROW_ROUTE(app, "/chatbot")
        .methods(crow::HTTPMethod::POST)
        ([](const crow::request& req) {
            crow::json::rvalue body;
            try {
                body = crow::json::load(req.body);
            } catch (const std::exception& e) {
                return crow::response(400, R"({"error": "Invalid JSON request"})");
            }
            
            if (!body.has("question")) {
                return crow::response(400, R"({"error": "Missing 'question' field"})");
            }
            
            std::string question = body["question"].s();
            std::string answer = process_query(question);
            
            crow::json::wvalue result;
            result["response"] = answer;
            
            return crow::response(200, result);
        });
    
    // Health check endpoint
    CROW_ROUTE(app, "/health")
        .methods(crow::HTTPMethod::GET)
        ([]() {
            crow::json::wvalue result;
            result["status"] = "healthy";
            return crow::response(200, result);
        });
    
    // Frontend endpoint
    CROW_ROUTE(app, "/")
        ([]() {
            std::string html = 
                "<!DOCTYPE html>"
                "<html lang=\"es\">"
                "<head>"
                "    <meta charset=\"UTF-8\">"
                "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                "    <title>IA MIGRANTE - Asistente de Inmigración</title>"
                "    <style>"
                "        body { font-family: Arial, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }"
                "        .chat-container { border: 1px solid #ddd; border-radius: 8px; padding: 20px; height: 400px; overflow-y: auto; }"
                "        .input-container { display: flex; margin-top: 20px; }"
                "        #message-input { flex-grow: 1; padding: 10px; }"
                "        button { padding: 10px 20px; background: #0066cc; color: white; border: none; margin-left: 10px; cursor: pointer; }"
                "        .message { margin-bottom: 10px; padding: 10px; border-radius: 5px; }"
                "        .user-message { background-color: #e6f7ff; text-align: right; }"
                "        .bot-message { background-color: #f2f2f2; }"
                "    </style>"
                "</head>"
                "<body>"
                "    <h1>🚀 IA MIGRANTE - Asistente de Inmigración</h1>"
                "    <div class=\"chat-container\" id=\"chat-container\">"
                "        <div class=\"message bot-message\">¡Hola! Soy IA MIGRANTE, tu asistente de inmigración. ¿En qué puedo ayudarte hoy?</div>"
                "    </div>"
                "    <div class=\"input-container\">"
                "        <input type=\"text\" id=\"message-input\" placeholder=\"Escribe tu pregunta aquí...\">"
                "        <button onclick=\"sendMessage()\">Enviar</button>"
                "    </div>"
                "    <script>"
                "        function sendMessage() {"
                "            const input = document.getElementById('message-input');"
                "            const message = input.value.trim();"
                "            "
                "            if (message.length === 0) return;"
                "            "
                "            // Display user message"
                "            addMessage(message, 'user');"
                "            input.value = '';"
                "            "
                "            // Call API"
                "            fetch('/chatbot', {"
                "                method: 'POST',"
                "                headers: { 'Content-Type': 'application/json' },"
                "                body: JSON.stringify({ question: message })"
                "            })"
                "            .then(response => response.json())"
                "            .then(data => {"
                "                addMessage(data.response, 'bot');"
                "            })"
                "            .catch(error => {"
                "                addMessage('Lo siento, ha ocurrido un error. Por favor, intenta de nuevo más tarde.', 'bot');"
                "                console.error('Error:', error);"
                "            });"
                "        }"
                "        "
                "        function addMessage(text, sender) {"
                "            const chatContainer = document.getElementById('chat-container');"
                "            const messageDiv = document.createElement('div');"
                "            messageDiv.classList.add('message');"
                "            messageDiv.classList.add(sender + '-message');"
                "            messageDiv.textContent = text;"
                "            chatContainer.appendChild(messageDiv);"
                "            chatContainer.scrollTop = chatContainer.scrollHeight;"
                "        }"
                "        "
                "        // Allow Enter key to send messages"
                "        document.getElementById('message-input').addEventListener('keypress', function(e) {"
                "            if (e.key === 'Enter') {"
                "                sendMessage();"
                "            }"
                "        });"
                "    </script>"
                "</body>"
                "</html>";
            
            return crow::response(html);
        });
    
    // Start the server
    log_info("Iniciando servidor en puerto 8080");
    app.port(8080).multithreaded().run();
    
    // Clean up on exit
    cleanup_resources();
    
    return 0;
}
