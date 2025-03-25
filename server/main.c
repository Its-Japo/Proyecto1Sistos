#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <cjson/cJSON.h>

// User structure
typedef struct Message {
    char *data;
    struct Message *next;
} Message;

typedef struct User {
    pthread_mutex_t mutex;
    char *username;
    char *ip_address;
    char *status;
    struct lws *wsi;
    time_t last_activity_time;
    Message *message_queue_head;
    Message *message_queue_tail;
    struct User *next;
} User;

// Global list of users
static User *users = NULL;
static pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;

// Add message to user's queue
static void queue_message(User *user, const char *message) {
    Message *msg = malloc(sizeof(Message));
    msg->data = strdup(message);
    msg->next = NULL;

    pthread_mutex_lock(&user->mutex);
    if (!user->message_queue_tail) {
        user->message_queue_head = user->message_queue_tail = msg;
    } else {
        user->message_queue_tail->next = msg;
        user->message_queue_tail = msg;
    }
    lws_callback_on_writable(user->wsi);
    pthread_mutex_unlock(&user->mutex);
}

// Broadcast message to all users
static void broadcast_message(const char *message, User *exclude) {
    pthread_mutex_lock(&users_mutex);
    User *current = users;
    while (current) {
        if (current != exclude) {
            queue_message(current, message);
        }
        current = current->next;
    }
    pthread_mutex_unlock(&users_mutex);
}

// Find user by username
static User *find_user_by_name(const char *username) {
    pthread_mutex_lock(&users_mutex);
    User *current = users;
    while (current) {
        if (current->username && strcmp(current->username, username) == 0) {
            pthread_mutex_unlock(&users_mutex);
            return current;
        }
        current = current->next;
    }
    pthread_mutex_unlock(&users_mutex);
    return NULL;
}

// Get user list as JSON array
static cJSON *get_user_list() {
    cJSON *list = cJSON_CreateArray();
    pthread_mutex_lock(&users_mutex);
    User *current = users;
    while (current) {
        if (current->username) {
            cJSON_AddItemToArray(list, cJSON_CreateString(current->username));
        }
        current = current->next;
    }
    pthread_mutex_unlock(&users_mutex);
    return list;
}

// Generate timestamp
static char *get_timestamp() {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char *buf = malloc(20);
    strftime(buf, 20, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

// Send error message
static void send_error(struct lws *wsi, User *user, const char *description) {
    cJSON *error = cJSON_CreateObject();
    char *timestamp = get_timestamp();
    cJSON_AddStringToObject(error, "type", "error");
    cJSON_AddStringToObject(error, "sender", "server");
    cJSON_AddStringToObject(error, "content", description);
    cJSON_AddStringToObject(error, "timestamp", timestamp);
    char *json_str = cJSON_PrintUnformatted(error);
    queue_message(user, json_str);
    free(json_str);
    free(timestamp);
    cJSON_Delete(error);
}

// WebSocket callback
static int callback_chat(struct lws *wsi, enum lws_callback_reasons reason, void *user_data, void *in, size_t len) {
    User *user = (User *)user_data;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            user = malloc(sizeof(User));
            pthread_mutex_init(&user->mutex, NULL);
            user->username = NULL;
            user->status = strdup("ACTIVO");
            user->wsi = wsi;
            user->last_activity_time = time(NULL);
            user->message_queue_head = user->message_queue_tail = NULL;
            user->next = NULL;

            char ip[INET6_ADDRSTRLEN];
            lws_get_peer_simple(wsi, ip, sizeof(ip));
            user->ip_address = strdup(ip);

            pthread_mutex_lock(&users_mutex);
            user->next = users;
            users = user;
            pthread_mutex_unlock(&users_mutex);

            lws_set_wsi_user(wsi, user);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            if (!user || len == 0) break;

            pthread_mutex_lock(&user->mutex);
            user->last_activity_time = time(NULL);
            pthread_mutex_unlock(&user->mutex);

            cJSON *msg = cJSON_ParseWithLength((char *)in, len);
            if (!msg) {
                send_error(wsi, user, "Invalid JSON");
                break;
            }

            const cJSON *type = cJSON_GetObjectItem(msg, "type");
            const cJSON *sender = cJSON_GetObjectItem(msg, "sender");
            if (!type || !type->valuestring || (sender && !sender->valuestring)) {
                send_error(wsi, user, "Missing or invalid fields");
                cJSON_Delete(msg);
                break;
            }

            if (!user->username) {
                if (strcmp(type->valuestring, "register") != 0) {
                    send_error(wsi, user, "Must register first");
                } else if (!sender || !sender->valuestring) {
                    send_error(wsi, user, "Missing sender for registration");
                } else if (find_user_by_name(sender->valuestring)) {
                    send_error(wsi, user, "Username already taken");
                } else {
                    pthread_mutex_lock(&user->mutex);
                    user->username = strdup(sender->valuestring);
                    pthread_mutex_unlock(&user->mutex);

                    cJSON *response = cJSON_CreateObject();
                    char *timestamp = get_timestamp();
                    cJSON_AddStringToObject(response, "type", "register_success");
                    cJSON_AddStringToObject(response, "sender", "server");
                    cJSON_AddStringToObject(response, "content", "Registro exitoso");
                    cJSON_AddItemToObject(response, "userList", get_user_list());
                    cJSON_AddStringToObject(response, "timestamp", timestamp);
                    char *json_str = cJSON_PrintUnformatted(response);
                    queue_message(user, json_str);

                    cJSON *connected = cJSON_CreateObject();
                    cJSON_AddStringToObject(connected, "type", "user_connected");
                    cJSON_AddStringToObject(connected, "sender", "server");
                    cJSON_AddStringToObject(connected, "content", user->username);
                    cJSON_AddStringToObject(connected, "timestamp", timestamp);
                    char *connected_str = cJSON_PrintUnformatted(connected);
                    broadcast_message(connected_str, user);

                    free(json_str);
                    free(connected_str);
                    free(timestamp);
                    cJSON_Delete(response);
                    cJSON_Delete(connected);
                }
            } else {
                if (strcmp(type->valuestring, "broadcast") == 0) {
                    const cJSON *content = cJSON_GetObjectItem(msg, "content");
                    if (content && content->valuestring) {
                        cJSON *broadcast = cJSON_CreateObject();
                        char *timestamp = get_timestamp();
                        cJSON_AddStringToObject(broadcast, "type", "broadcast");
                        cJSON_AddStringToObject(broadcast, "sender", user->username);
                        cJSON_AddStringToObject(broadcast, "content", content->valuestring);
                        cJSON_AddStringToObject(broadcast, "timestamp", timestamp);
                        char *json_str = cJSON_PrintUnformatted(broadcast);
                        broadcast_message(json_str, NULL);
                        free(json_str);
                        free(timestamp);
                        cJSON_Delete(broadcast);
                    }
                } else if (strcmp(type->valuestring, "private") == 0) {
                    const cJSON *target = cJSON_GetObjectItem(msg, "target");
                    const cJSON *content = cJSON_GetObjectItem(msg, "content");
                    if (target && target->valuestring && content && content->valuestring) {
                        User *target_user = find_user_by_name(target->valuestring);
                        if (target_user) {
                            cJSON *private = cJSON_CreateObject();
                            char *timestamp = get_timestamp();
                            cJSON_AddStringToObject(private, "type", "private");
                            cJSON_AddStringToObject(private, "sender", user->username);
                            cJSON_AddStringToObject(private, "target", target->valuestring);
                            cJSON_AddStringToObject(private, "content", content->valuestring);
                            cJSON_AddStringToObject(private, "timestamp", timestamp);
                            char *json_str = cJSON_PrintUnformatted(private);
                            queue_message(target_user, json_str);
                            free(json_str);
                            free(timestamp);
                            cJSON_Delete(private);
                        } else {
                            send_error(wsi, user, "Target user not found");
                        }
                    }
                } else if (strcmp(type->valuestring, "list_users") == 0) {
                    cJSON *response = cJSON_CreateObject();
                    char *timestamp = get_timestamp();
                    cJSON_AddStringToObject(response, "type", "list_users_response");
                    cJSON_AddStringToObject(response, "sender", "server");
                    cJSON_AddItemToObject(response, "content", get_user_list());
                    cJSON_AddStringToObject(response, "timestamp", timestamp);
                    char *json_str = cJSON_PrintUnformatted(response);
                    queue_message(user, json_str);
                    free(json_str);
                    free(timestamp);
                    cJSON_Delete(response);
                } else if (strcmp(type->valuestring, "user_info") == 0) {
                    const cJSON *target = cJSON_GetObjectItem(msg, "target");
                    if (target && target->valuestring) {
                        User *target_user = find_user_by_name(target->valuestring);
                        if (target_user) {
                            cJSON *response = cJSON_CreateObject();
                            cJSON *content = cJSON_CreateObject();
                            char *timestamp = get_timestamp();
                            cJSON_AddStringToObject(response, "type", "user_info_response");
                            cJSON_AddStringToObject(response, "sender", "server");
                            cJSON_AddStringToObject(response, "target", target->valuestring);
                            cJSON_AddStringToObject(content, "ip", target_user->ip_address);
                            cJSON_AddStringToObject(content, "status", target_user->status);
                            cJSON_AddItemToObject(response, "content", content);
                            cJSON_AddStringToObject(response, "timestamp", timestamp);
                            char *json_str = cJSON_PrintUnformatted(response);
                            queue_message(user, json_str);
                            free(json_str);
                            free(timestamp);
                            cJSON_Delete(response);
                        } else {
                            send_error(wsi, user, "User not found");
                        }
                    }
                } else if (strcmp(type->valuestring, "change_status") == 0) {
                    const cJSON *content = cJSON_GetObjectItem(msg, "content");
                    if (content && content->valuestring) {
                        const char *new_status = content->valuestring;
                        if (strcmp(new_status, "ACTIVO") == 0 || strcmp(new_status, "OCUPADO") == 0 || strcmp(new_status, "INACTIVO") == 0) {
                            pthread_mutex_lock(&user->mutex);
                            free(user->status);
                            user->status = strdup(new_status);
                            pthread_mutex_unlock(&user->mutex);

                            cJSON *update = cJSON_CreateObject();
                            cJSON *content_obj = cJSON_CreateObject();
                            char *timestamp = get_timestamp();
                            cJSON_AddStringToObject(update, "type", "status_update");
                            cJSON_AddStringToObject(update, "sender", "server");
                            cJSON_AddStringToObject(content_obj, "user", user->username);
                            cJSON_AddStringToObject(content_obj, "status", new_status);
                            cJSON_AddItemToObject(update, "content", content_obj);
                            cJSON_AddStringToObject(update, "timestamp", timestamp);
                            char *json_str = cJSON_PrintUnformatted(update);
                            broadcast_message(json_str, NULL);
                            free(json_str);
                            free(timestamp);
                            cJSON_Delete(update);
                        }
                    }
                } else if (strcmp(type->valuestring, "disconnect") == 0) {
                    cJSON *disconnected = cJSON_CreateObject();
                    char *timestamp = get_timestamp();
                    cJSON_AddStringToObject(disconnected, "type", "user_disconnected");
                    cJSON_AddStringToObject(disconnected, "sender", "server");
                    char disconnected_content[256];
                    snprintf(disconnected_content, sizeof(disconnected_content), "%s ha salido", user->username);
                    cJSON_AddStringToObject(disconnected, "content", disconnected_content);
                    cJSON_AddStringToObject(disconnected, "timestamp", timestamp);
                    char *json_str = cJSON_PrintUnformatted(disconnected);
                    broadcast_message(json_str, user);
                    free(json_str);
                    free(timestamp);
                    cJSON_Delete(disconnected);
                    // Connection will close after this message is sent
                }
            }
            cJSON_Delete(msg);
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (!user) break;
            pthread_mutex_lock(&user->mutex);
            Message *msg = user->message_queue_head;
            if (msg) {
                user->message_queue_head = msg->next;
                if (!user->message_queue_head) {
                    user->message_queue_tail = NULL;
                }
                lws_write(wsi, (unsigned char *)msg->data, strlen(msg->data), LWS_WRITE_TEXT);
                free(msg->data);
                free(msg);
                if (user->message_queue_head) {
                    lws_callback_on_writable(wsi);
                }
            }
            pthread_mutex_unlock(&user->mutex);
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (!user) break;
            pthread_mutex_lock(&users_mutex);
            User **current = &users;
            while (*current && *current != user) {
                current = &(*current)->next;
            }
            if (*current) {
                *current = user->next;
            }
            pthread_mutex_unlock(&users_mutex);

            if (user->username) {
                cJSON *disconnected = cJSON_CreateObject();
                char *timestamp = get_timestamp();
                cJSON_AddStringToObject(disconnected, "type", "user_disconnected");
                cJSON_AddStringToObject(disconnected, "sender", "server");
                char disconnected_content[256];
                snprintf(disconnected_content, sizeof(disconnected_content), "%s ha salido", user->username);
                cJSON_AddStringToObject(disconnected, "content", disconnected_content);
                cJSON_AddStringToObject(disconnected, "timestamp", timestamp);
                char *json_str = cJSON_PrintUnformatted(disconnected);
                broadcast_message(json_str, user);
                free(json_str);
                free(timestamp);
                cJSON_Delete(disconnected);
            }

            pthread_mutex_lock(&user->mutex);
            free(user->username);
            free(user->ip_address);
            free(user->status);
            while (user->message_queue_head) {
                Message *msg = user->message_queue_head;
                user->message_queue_head = msg->next;
                free(msg->data);
                free(msg);
            }
            pthread_mutex_unlock(&user->mutex);
            pthread_mutex_destroy(&user->mutex);
            free(user);
            break;
        }

        default:
            break;
    }
    return 0;
}
 
static void *inactivity_check(void *arg) {
    while (1) {
        sleep(10);
        time_t now = time(NULL);
        pthread_mutex_lock(&users_mutex);
        User *current = users;
        while (current) {
            pthread_mutex_lock(&current->mutex);
            if (current->username && strcmp(current->status, "INACTIVO") != 0 && (now - current->last_activity_time) > 60) {
                free(current->status);
                current->status = strdup("INACTIVO");
                pthread_mutex_unlock(&current->mutex);

                cJSON *update = cJSON_CreateObject();
                cJSON *content = cJSON_CreateObject();
                char *timestamp = get_timestamp();
                cJSON_AddStringToObject(update, "type", "status_update");
                cJSON_AddStringToObject(update, "sender", "server");
                cJSON_AddStringToObject(content, "user", current->username);
                cJSON_AddStringToObject(content, "status", "INACTIVO");
                cJSON_AddItemToObject(update, "content", content);
                cJSON_AddStringToObject(update, "timestamp", timestamp);
                char *json_str = cJSON_PrintUnformatted(update);
                broadcast_message(json_str, NULL);
                free(json_str);
                free(timestamp);
                cJSON_Delete(update);
            } else {
                pthread_mutex_unlock(&current->mutex);
            }
            current = current->next;
        }
        pthread_mutex_unlock(&users_mutex);
    }
    return NULL;
}

static struct lws_protocols protocols[] = {
    {"chat", callback_chat, sizeof(User), 0},
    {NULL, NULL, 0, 0}
};

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.protocols = protocols;
    info.count_threads = 4;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    pthread_t inactivity_thread;
    pthread_create(&inactivity_thread, NULL, inactivity_check, NULL);
    pthread_detach(inactivity_thread);

    printf("Server started on port %d\n", port);
    while (1) {
        lws_service(context, 0);
    }

    lws_context_destroy(context);
    return 0;
}
