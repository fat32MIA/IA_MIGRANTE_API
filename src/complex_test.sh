#!/bin/bash

# Script para pruebas intensivas de IA_MIGRANTE con preguntas complejas

# Configuración
IA_MIGRANTE_PATH="./ia_migrante"
RESULTS_DIR="./test_results"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULTS_FILE="${RESULTS_DIR}/complex_test_results_${TIMESTAMP}.txt"

# Asegurar que existe el directorio de resultados
mkdir -p "${RESULTS_DIR}"

# Crear el archivo de resultados
echo "PRUEBAS AVANZADAS DE IA_MIGRANTE" > "${RESULTS_FILE}"
echo "Fecha: $(date)" >> "${RESULTS_FILE}"
echo "===================================================" >> "${RESULTS_FILE}"

# Función para ejecutar una consulta y guardar resultados
run_test() {
    local query="$1"
    local description="$2"
    
    echo -e "\n\n===== PRUEBA: ${description} =====" >> "${RESULTS_FILE}"
    echo "CONSULTA: ${query}" >> "${RESULTS_FILE}"
    echo "-------------------------------------------------" >> "${RESULTS_FILE}"
    
    # Ejecutar la consulta
    echo -e "\nEjecutando: ${description}"
    echo -e "Consulta: ${query}"
    
    # Capturar salida
    result=$(${IA_MIGRANTE_PATH} "${query}")
    
    # Guardar resultado
    echo "${result}" >> "${RESULTS_FILE}"
    
    # Extraer solo la respuesta para mostrar un resumen
    answer=$(echo "${result}" | sed -n '/Respuesta:/,$p' | tail -n +2)
    
    # Mostrar un resumen de la respuesta (primeros 100 caracteres)
    echo -e "Respuesta (resumen): ${answer:0:100}...\n"
}

echo "Iniciando pruebas complejas de IA_MIGRANTE..."
echo "Los resultados se guardarán en ${RESULTS_FILE}"

# 1. Preguntas de ajuste de estatus TPS-EB1
run_test "¿Cuáles son las alternativas legales para una persona que entró con visa B2, estuvo sin estatus por 3 años, obtuvo TPS y ahora es cónyuge de beneficiario principal de EB1?" \
         "TPS-EB1 con largo período sin estatus"

run_test "Si una persona tiene TPS pero estuvo sin estatus por más de 180 días antes, ¿puede aplicar la sección 245(k) si es beneficiario de EB1 o EB2?" \
         "Aplicabilidad de 245(k) con TPS"

run_test "¿Qué excepciones existen a la regla de 180 días para la sección 245(k) en casos de ajuste por empleo?" \
         "Excepciones a 245(k)"

# 2. Preguntas sobre visas especializadas
run_test "Para una visa O-1, ¿qué evidencia específica necesita presentar un chef reconocido internacionalmente pero sin premios formales?" \
         "Visa O-1 para chef sin premios"

run_test "¿Cómo afecta el cambio de empleador a una petición de Green Card en proceso a través de PERM cuando se encuentra en el paso I-140?" \
         "Cambio de empleador durante proceso PERM"

# 3. Preguntas sobre casos específicos de asilo
run_test "¿Un solicitante de asilo puede viajar a un tercer país (no su país de origen) mientras su caso está pendiente?" \
         "Viaje durante proceso de asilo"

run_test "¿Cuáles son las implicaciones de un arresto por DUI para un solicitante de asilo cuyo caso lleva 2 años pendiente?" \
         "DUI durante proceso de asilo"

# 4. Preguntas sobre inmigración familiar con complicaciones
run_test "Si un ciudadano estadounidense solicita a su hermano pero este fallece durante el proceso, ¿pueden los hijos del hermano reclamar algún beneficio migratorio?" \
         "Fallecimiento del beneficiario en petición familiar"

run_test "¿Qué ocurre con una petición I-130 para un cónyuge si el matrimonio termina durante el proceso pero hay hijos en común?" \
         "Divorcio durante proceso I-130"

# 5. Preguntas sobre casos complejos de inadmisibilidad
run_test "¿Qué opciones tiene alguien con una orden de deportación in absentia de hace 10 años que ahora es cónyuge de ciudadano estadounidense?" \
         "Orden de deportación in absentia antigua"

run_test "¿Cómo afecta un cargo por posesión simple de marihuana (menos de 30g) de hace 15 años a una solicitud actual de naturalización?" \
         "Cargo antiguo de marihuana para naturalización"

# 6. Preguntas en inglés
run_test "What are the specific requirements and evidence needed for an employment-based National Interest Waiver for a medical researcher working on infectious diseases?" \
         "National Interest Waiver para investigador médico (EN)"

run_test "How does a previous J-1 visa with a two-year home residency requirement affect a marriage-based green card application if the applicant never fulfilled the requirement?" \
         "J-1 con requisito de residencia no cumplido (EN)"

# 7. Preguntas sobre situaciones actuales
run_test "¿Cómo afecta tener DACA a una petición familiar si el beneficiario es cónyuge de ciudadano y entró sin inspección?" \
         "DACA con entrada sin inspección"

run_test "¿Cuáles son las opciones para un venezolano que llegó a la frontera en 2023, tiene un patrocinador pero no calificó para parole humanitario?" \
         "Venezolano sin parole humanitario"

# 8. Preguntas sobre procesos avanzados de ciudadanía
run_test "¿En qué circunstancias un LPR puede solicitar la ciudadanía antes de los 5 años si no está casado con ciudadano?" \
         "Naturalización acelerada sin matrimonio"

run_test "¿Cómo afecta un largo período fuera de EE.UU. (más de 6 meses pero menos de 1 año) a una solicitud de naturalización?" \
         "Ausencias largas para naturalización"

echo -e "\nPruebas completadas. Resultados guardados en: ${RESULTS_FILE}"
echo "Puedes revisar el archivo para análisis detallado."
