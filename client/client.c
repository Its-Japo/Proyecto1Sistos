/*
 * client.c
 *
 * Ejemplo de cliente para sistema de chat basado en WebSocket.
 *
 * Uso:
 *   ./client <nombre_de_usuario> <IP_del_servidor> <puerto_del_servidor>
 *
 * Comandos disponibles (ingresados por STDIN):
 *   broadcast <mensaje>            : Enviar mensaje a todos los usuarios.
 *   private <usuario> <mensaje>    : Enviar mensaje privado a un usuario.
 *   status <ACTIVO|OCUPADO|INACTIVO>: Cambiar el estado del usuario.
 *   list                         : Solicitar listado de usuarios conectados.
 *   info <usuario>               : Solicitar información de un usuario.
 *   help                         : Mostrar ayuda.
 *   exit                         : Desconectarse y salir.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <libwebsockets.h>
#include <cjson/cJSON.h>

#define BUFFER_SIZE 1024

// Variables globales
char *global_username = NULL;
struct lws *client_wsi = NULL;
struct lws_context *context = NULL;
volatile int should_exit = 0;

// Estructura para la cola de mensajes salientes
typedef struct Message {
    char *data;
    struct Message *next;
} Message;

static Message *msg_queue_head = NULL;
static Message *msg_queue_tail = NULL;
pthread_mutex_t msg_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// Función para obtener el timestamp en formato ISO 8601
char *get_timestamp() {
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char *buf = malloc(21);
    strftime(buf, 21, "%Y-%m-%dT%H:%M:%SZ", tm_info);
    return buf;
}

// Agrega un mensaje a la cola de mensajes
void queue_message(const char *msg) {
    Message *m = malloc(sizeof(Message));
    m->data = strdup(msg);
    m->next = NULL;
    pthread_mutex_lock(&msg_queue_mutex);
    if (!msg_queue_tail) {
        msg_queue_head = msg_queue_tail = m;
    } else {
        msg_queue_tail->next = m;
        msg_queue_tail = m;
    }
    pthread_mutex_unlock(&msg_queue_mutex);
    // Solicita al WS que se active el callback de escritura
    if (client_wsi)
         lws_callback_on_writable(client_wsi);
}

// Callback del cliente (maneja conexión, recepción, escritura y cierre)
static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,
                             void *user, void *in, size_t len) {
    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            printf("Conexión establecida con el servidor.\n");
            client_wsi = wsi;
            // Enviar mensaje de registro
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "register");
            cJSON_AddStringToObject(json, "sender", global_username);
            cJSON_AddNullToObject(json, "content");
            char *timestamp = get_timestamp();
            cJSON_AddStringToObject(json, "timestamp", timestamp);
            free(timestamp);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
            break;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            char *data = (char *)in;
            // Obtener la hora local (hora y minuto)
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            char time_str[16];
            strftime(time_str, sizeof(time_str), "[%H:%M]", t);

            cJSON *json = cJSON_Parse(data);
            if (json) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
                if (cJSON_IsString(type) && (type->valuestring != NULL)) {
                    if (strcmp(type->valuestring, "broadcast") == 0) {
                        const cJSON *sender = cJSON_GetObjectItemCaseSensitive(json, "sender");
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Broadcast] [%s]: %s\n", time_str, sender->valuestring, content->valuestring);
                    }
                    else if (strcmp(type->valuestring, "private") == 0) {
                        const cJSON *sender = cJSON_GetObjectItemCaseSensitive(json, "sender");
                        const cJSON *target = cJSON_GetObjectItemCaseSensitive(json, "target");
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Privado] [%s] → [%s]: %s\n", time_str, sender->valuestring, target->valuestring, content->valuestring);
                    }
                    else if (strcmp(type->valuestring, "status_update") == 0) {
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        const cJSON *user = cJSON_GetObjectItemCaseSensitive(content, "user");
                        const cJSON *status = cJSON_GetObjectItemCaseSensitive(content, "status");
                        printf("%s [Estado] [%s] cambió su estado a %s\n", time_str, user->valuestring, status->valuestring);
                    }
                    else if (strcmp(type->valuestring, "list_users_response") == 0) {
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Usuarios Conectados]: ", time_str);
                        int size = cJSON_GetArraySize(content);
                        for (int i = 0; i < size; i++) {
                            cJSON *item = cJSON_GetArrayItem(content, i);
                            if (item && cJSON_IsString(item)) {
                                printf("%s ", item->valuestring);
                            }
                        }
                        printf("\n");
                    }
                    else if (strcmp(type->valuestring, "user_info_response") == 0) {
                        const cJSON *target = cJSON_GetObjectItemCaseSensitive(json, "target");
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(content, "ip");
                        const cJSON *status = cJSON_GetObjectItemCaseSensitive(content, "status");
                        printf("%s [Info] [Usuario]: %s, [IP]: %s, [Estado]: %s\n", time_str, target->valuestring, ip->valuestring, status->valuestring);
                    }
                    else if (strcmp(type->valuestring, "register_success") == 0) {
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Registro] %s\n", time_str, content->valuestring);
                    }
                    else if (strcmp(type->valuestring, "user_connected") == 0) {
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Conexión] Usuario %s se ha conectado.\n", time_str, content->valuestring);
                    }
                    else if (strcmp(type->valuestring, "user_disconnected") == 0) {
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Desconexión] %s\n", time_str, content->valuestring);
                    }
                    else if (strcmp(type->valuestring, "error") == 0) {
                        const cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
                        printf("%s [Error] %s\n", time_str, content->valuestring);
                    }
                    else {
                        // Si se recibe un tipo no reconocido, se muestra el mensaje completo.
                        printf("%s [Mensaje] %s\n", time_str, data);
                    }
                } else {
                    printf("%s Mensaje recibido (formato desconocido): %s\n", time_str, data);
                }
                cJSON_Delete(json);
            } else {
                printf("%s Mensaje recibido: %s\n", time_str, data);
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            pthread_mutex_lock(&msg_queue_mutex);
            if (msg_queue_head) {
                size_t msg_len = strlen(msg_queue_head->data);
                // Reservar espacio extra LWS_PRE
                unsigned char *buf = malloc(LWS_PRE + msg_len);
                memcpy(buf + LWS_PRE, msg_queue_head->data, msg_len);
                int n = lws_write(wsi, buf + LWS_PRE, msg_len, LWS_WRITE_TEXT);
                if (n < (int)msg_len) {
                    printf("Error al enviar mensaje.\n");
                }
                free(buf);
                Message *old = msg_queue_head;
                msg_queue_head = msg_queue_head->next;
                if (!msg_queue_head)
                    msg_queue_tail = NULL;
                free(old->data);
                free(old);
            }
            pthread_mutex_unlock(&msg_queue_mutex);
            // Si hay más mensajes en cola, solicitar otro callback
            pthread_mutex_lock(&msg_queue_mutex);
            if (msg_queue_head) {
                lws_callback_on_writable(wsi);
            }
            pthread_mutex_unlock(&msg_queue_mutex);
            break;
        }
        case LWS_CALLBACK_CLOSED: {
            printf("Conexión cerrada.\n");
            client_wsi = NULL;
            should_exit = 1;
            break;
        }
        default:
            break;
    }
    return 0;
}

// Definición del protocolo para el cliente
static struct lws_protocols protocols[] = {
    { "chat-protocol", callback_client, 0, BUFFER_SIZE },
    { NULL, NULL, 0, 0 }
};

// Hilo para leer comandos desde la terminal
void *input_thread(void *arg) {
    char line[BUFFER_SIZE];
    while (!should_exit) {
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        // Eliminar salto de línea
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0)
            continue;
        
        // Separa la línea en tokens para identificar el comando
        char *command = strtok(line, " ");
        if (!command)
            continue;
        
        if (strcmp(command, "broadcast") == 0) {
            char *msg_content = strtok(NULL, "");
            if (!msg_content) {
                printf("Uso: broadcast <mensaje>\n");
                continue;
            }
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "broadcast");
            cJSON_AddStringToObject(json, "sender", global_username);
            cJSON_AddStringToObject(json, "content", msg_content);
            char *timestamp = get_timestamp();
            cJSON_AddStringToObject(json, "timestamp", timestamp);
            free(timestamp);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
        } else if (strcmp(command, "private") == 0) {
            char *target = strtok(NULL, " ");
            char *msg_content = strtok(NULL, "");
            if (!target || !msg_content) {
                printf("Uso: private <usuario> <mensaje>\n");
                continue;
            }
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "private");
            cJSON_AddStringToObject(json, "sender", global_username);
            cJSON_AddStringToObject(json, "target", target);
            cJSON_AddStringToObject(json, "content", msg_content);
            char *timestamp = get_timestamp();
            cJSON_AddStringToObject(json, "timestamp", timestamp);
            free(timestamp);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
        } else if (strcmp(command, "status") == 0) {
            char *new_status = strtok(NULL, " ");
            if (!new_status) {
                printf("Uso: status <ACTIVO|OCUPADO|INACTIVO>\n");
                continue;
            }
            if (strcmp(new_status, "ACTIVO") != 0 && strcmp(new_status, "OCUPADO") != 0 &&
                strcmp(new_status, "INACTIVO") != 0) {
                printf("Estado inválido. Use: ACTIVO, OCUPADO o INACTIVO.\n");
                continue;
            }
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "change_status");
            cJSON_AddStringToObject(json, "sender", global_username);
            cJSON_AddStringToObject(json, "content", new_status);
            char *timestamp = get_timestamp();
            cJSON_AddStringToObject(json, "timestamp", timestamp);
            free(timestamp);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
        } else if (strcmp(command, "list") == 0) {
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "list_users");
            cJSON_AddStringToObject(json, "sender", global_username);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
        } else if (strcmp(command, "info") == 0) {
            char *target = strtok(NULL, " ");
            if (!target) {
                printf("Uso: info <usuario>\n");
                continue;
            }
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "user_info");
            cJSON_AddStringToObject(json, "sender", global_username);
            cJSON_AddStringToObject(json, "target", target);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
        } else if (strcmp(command, "help") == 0) {
            printf("Comandos disponibles:\n");
            printf("  broadcast <mensaje>            - Enviar mensaje a todos\n");
            printf("  private <usuario> <mensaje>    - Enviar mensaje privado\n");
            printf("  status <ACTIVO|OCUPADO|INACTIVO> - Cambiar estado\n");
            printf("  list                         - Listar usuarios conectados\n");
            printf("  info <usuario>               - Mostrar información de un usuario\n");
            printf("  help                         - Mostrar esta ayuda\n");
            printf("  exit                         - Salir del chat\n");
        } else if (strcmp(command, "exit") == 0) {
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "type", "disconnect");
            cJSON_AddStringToObject(json, "sender", global_username);
            cJSON_AddStringToObject(json, "content", "Cierre de sesión");
            char *timestamp = get_timestamp();
            cJSON_AddStringToObject(json, "timestamp", timestamp);
            free(timestamp);
            char *msg = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            queue_message(msg);
            free(msg);
            should_exit = 1;
            break;
        } else {
            printf("Comando desconocido. Escriba 'help' para ver los comandos disponibles.\n");
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <nombre_de_usuario> <IP_del_servidor> <puerto_del_servidor>\n", argv[0]);
        return 1;
    }
    global_username = argv[1];
    const char *server_address = argv[2];
    int server_port = atoi(argv[3]);

    // Configuración del contexto libwebsockets (sin servidor de escucha)
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Error al crear el contexto libwebsockets.\n");
        return 1;
    }

    // Configuración de la conexión cliente
    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = context;
    ccinfo.address = server_address;
    ccinfo.port = server_port;
    ccinfo.path = "/chat";
    ccinfo.host = server_address;
    ccinfo.origin = server_address;
    ccinfo.protocol = protocols[0].name;

    if (!lws_client_connect_via_info(&ccinfo)) {
        fprintf(stderr, "Error al conectar con el servidor.\n");
        lws_context_destroy(context);
        return 1;
    }

    // Crear hilo para leer entrada de usuario
    pthread_t tid;
    pthread_create(&tid, NULL, input_thread, NULL);

    // Bucle principal del servicio WebSocket
    while (!should_exit) {
        lws_service(context, 50);
    }

    pthread_join(tid, NULL);
    lws_context_destroy(context);
    return 0;
}

