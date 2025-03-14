# IA MIGRANTE: Asistente Virtual de Inmigración

Un chatbot de código abierto especializado en responder preguntas sobre temas de inmigración, desarrollado por Héctor Nolivos.

![IA MIGRANTE Banner](https://via.placeholder.com/1200x300?text=IA+MIGRANTE)

## Descripción

IA MIGRANTE es un asistente virtual diseñado para proporcionar información sobre diversos aspectos de la inmigración, incluyendo visas, asilo, ciudadanía, deportación y más. El sistema utiliza una combinación de bases de conocimiento estructuradas y palabras clave para ofrecer respuestas precisas y completas a consultas relacionadas con la inmigración.

## Características

- 🌐 **API REST** para fácil integración en aplicaciones
- 🧠 **Base de conocimiento especializada** en temas migratorios
- 💬 **Interfaz web** simple e intuitiva para interacción directa
- 🔎 **Reconocimiento por palabras clave** para identificar temas específicos
- 📊 **Sistema de caché** para respuestas frecuentes

## Requisitos

- C++17 compatible compiler
- SQLite3
- Crow (incluido en el repositorio)
- nlohmann/json (para manejo de JSON)

## Instalación

### Método 1: Compilación manual

1. Clonar el repositorio:
   ```bash
   git clone https://github.com/hectornolivos/ia-migrante.git
   cd ia-migrante
   ```

2. Compilar el proyecto:
   ```bash
   g++ -std=c++17 chatbot_ia_razonamiento.cpp -o chatbot_ia \
     -I./Crow/include \
     -lsqlite3 -lpthread -ldl -O3
   ```

3. Ejecutar:
   ```bash
   ./chatbot_ia
   ```

### Método 2: Usando Docker

```bash
docker build -t ia-migrante .
docker run -p 8080:8080 ia-migrante
```

## Uso de la API

IA MIGRANTE expone una API REST que puede ser utilizada para integrar el asistente virtual en otras aplicaciones.

### Endpoint de Chat

**URL**: `/chatbot`  
**Método**: `POST`  
**Formato de solicitud**:

```json
{
  "question": "¿Cómo puedo solicitar asilo político?"
}
```

**Ejemplo de respuesta**:

```json
{
  "response": "El asilo se otorga a personas que tienen un temor fundado de persecución en su país de origen por motivos de raza, religión, nacionalidad, opinión política o pertenencia a un grupo social particular. El proceso generalmente implica una solicitud formal, entrevistas, y evaluación de evidencias. Durante el trámite, muchos países proporcionan autorización de trabajo temporal. Es importante buscar asesoramiento legal para el proceso de solicitud."
}
```

### Ejemplo de uso con cURL

```bash
curl -X POST http://localhost:8080/chatbot \
  -H "Content-Type: application/json" \
  -d '{"question":"¿Qué es una Green Card?"}'
```

### Ejemplo de uso con JavaScript

```javascript
async function consultarIAMigrante(pregunta) {
  const respuesta = await fetch('http://localhost:8080/chatbot', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({ question: pregunta })
  });
  
  const datos = await respuesta.json();
  return datos.response;
}

// Uso
consultarIAMigrante('¿Cómo funciona el Express Entry de Canadá?')
  .then(respuesta => console.log(respuesta));
```

### Ejemplo de uso con Python

```python
import requests

def consultar_ia_migrante(pregunta):
    respuesta = requests.post(
        'http://localhost:8080/chatbot',
        json={'question': pregunta}
    )
    return respuesta.json()['response']

# Uso
print(consultar_ia_migrante('¿Qué es DACA?'))
```

## Integración con aplicaciones existentes

IA MIGRANTE puede integrarse fácilmente en aplicaciones web, móviles o de escritorio existentes mediante su API REST:

### Aplicaciones web

Agrega un widget de chat a tu sitio web:

```html
<div id="ia-migrante-chat">
  <div id="chat-messages"></div>
  <div class="input-container">
    <input type="text" id="pregunta" placeholder="Haz una pregunta sobre inmigración...">
    <button onclick="enviarPregunta()">Enviar</button>
  </div>
</div>

<script>
  function enviarPregunta() {
    const pregunta = document.getElementById('pregunta').value;
    if (!pregunta) return;
    
    // Mostrar pregunta del usuario
    agregarMensaje('user', pregunta);
    
    // Consultar API
    fetch('http://tu-servidor:8080/chatbot', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ question: pregunta })
    })
    .then(response => response.json())
    .then(data => {
      agregarMensaje('bot', data.response);
    })
    .catch(error => {
      agregarMensaje('error', 'Lo siento, ocurrió un error al procesar tu pregunta.');
    });
    
    document.getElementById('pregunta').value = '';
  }
  
  function agregarMensaje(tipo, texto) {
    const mensajes = document.getElementById('chat-messages');
    const mensaje = document.createElement('div');
    mensaje.className = `mensaje ${tipo}`;
    mensaje.textContent = texto;
    mensajes.appendChild(mensaje);
    mensajes.scrollTop = mensajes.scrollHeight;
  }
</script>
```

### Aplicaciones móviles

Implementa llamadas a la API desde tus aplicaciones móviles:

#### Android (Kotlin)

```kotlin
private fun consultarIAMigrante(pregunta: String, callback: (String) -> Unit) {
    val url = "http://tu-servidor:8080/chatbot"
    val jsonObject = JSONObject()
    jsonObject.put("question", pregunta)
    
    val request = JsonObjectRequest(
        Request.Method.POST, url, jsonObject,
        { response ->
            callback(response.getString("response"))
        },
        { error ->
            callback("Error al procesar la consulta: ${error.message}")
        }
    )
    
    Volley.newRequestQueue(context).add(request)
}
```

#### iOS (Swift)

```swift
func consultarIAMigrante(pregunta: String, completion: @escaping (String?, Error?) -> Void) {
    let url = URL(string: "http://tu-servidor:8080/chatbot")!
    var request = URLRequest(url: url)
    request.httpMethod = "POST"
    request.addValue("application/json", forHTTPHeaderField: "Content-Type")
    
    let body: [String: Any] = ["question": pregunta]
    request.httpBody = try? JSONSerialization.data(withJSONObject: body)
    
    URLSession.shared.dataTask(with: request) { data, response, error in
        guard let data = data, error == nil else {
            completion(nil, error)
            return
        }
        
        do {
            if let jsonResponse = try JSONSerialization.jsonObject(with: data) as? [String: Any],
               let respuesta = jsonResponse["response"] as? String {
                completion(respuesta, nil)
            } else {
                completion(nil, NSError(domain: "", code: 0, userInfo: [NSLocalizedDescriptionKey: "Formato de respuesta inválido"]))
            }
        } catch {
            completion(nil, error)
        }
    }.resume()
}
```

## Temas soportados

IA MIGRANTE puede responder preguntas sobre una amplia variedad de temas relacionados con inmigración, incluyendo:

- **Visas**: Trabajo, estudio, turismo, etc.
- **Residencia permanente**: Green Card, Express Entry, arraigo
- **Asilo y refugio**: Procesos, derechos, protección temporal
- **Reunificación familiar**: Patrocinio de cónyuges, hijos, padres
- **Ciudadanía y naturalización**: Requisitos, procesos, doble nacionalidad
- **Problemas legales**: Deportación, antecedentes penales, opciones legales
- **Programas especiales**: DACA, VAWA, Visas U/T, TPS
- **Cambios de estatus**: Renovaciones, ajustes, situaciones de overstay

## Contribuir

Las contribuciones son bienvenidas. Para contribuir:

1. Haz un fork del repositorio
2. Crea una rama para tu funcionalidad (`git checkout -b feature/nueva-funcionalidad`)
3. Realiza tus cambios y haz commit (`git commit -am 'Agrega nueva funcionalidad'`)
4. Sube los cambios (`git push origin feature/nueva-funcionalidad`)
5. Crea un Pull Request

### Áreas para contribuir

- Ampliar la base de conocimiento
- Mejorar el sistema de detección de palabras clave
- Integrar modelos de lenguaje (LLM) para respuestas más contextuales
- Optimizar el rendimiento para mayor escalabilidad
- Añadir soporte para más idiomas

## Licencia

Este proyecto está licenciado bajo [MIT License](LICENSE).

## Autor

**Héctor Nolivos** - Desarrollador principal - [GitHub](https://github.com/hectornolivos)

## Agradecimientos

- A la comunidad de desarrolladores de C++ y Crow
- A todos los colaboradores que han aportado a este proyecto
- A las organizaciones que trabajan para mejorar la vida de los inmigrantes

---

¿Tienes preguntas o sugerencias? Abre un issue en este repositorio o contáctame directamente.
