#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

typedef struct UserCheck {
    char *username;
    char *status;
    struct lws *wsi;
    time_t last_activity_time; // Agregado para verificar inactividad
    pthread_mutex_t *mutex;
    struct UserCheck *next;
} UserCheck;

static User *users = NULL;
static pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;

static void queue_message(User *user, const char *message, int mutex_locked) {
    Message *msg = malloc(sizeof(Message));
    if (!msg) {
        fprintf(stderr, "Failed to allocate Message\n");
        return;
    }
    msg->data = strdup(message);
    if (!msg->data) {
        free(msg);
        fprintf(stderr, "Failed to duplicate message\n");
        return;
    }
    msg->next = NULL;

    if (!mutex_locked) {
        pthread_mutex_lock(&user->mutex);
    }

    int queue_size = 0;
    Message *temp = user->message_queue_head;
    while (temp) {
        queue_size++;
        temp = temp->next;
    }
    if (queue_size >= 100) {
        free(msg->data);
        free(msg);
        if (!mutex_locked) {
            pthread_mutex_unlock(&user->mutex);
        }
        fprintf(stderr, "Message queue full for user %s\n", user->username ? user->username : "unregistered");
        return;
    }

    if (!user->message_queue_tail) {
        user->message_queue_head = user->message_queue_tail = msg;
    } else {
        user->message_queue_tail->next = msg;
        user->message_queue_tail = msg;
    }
    if (lws_callback_on_writable(user->wsi) < 0) {
        fprintf(stderr, "Failed to request writable callback for %s\n", user->username ? user->username : "unregistered");
    }

    if (!mutex_locked) {
        pthread_mutex_unlock(&user->mutex);
    }
}

static void broadcast_message(const char *message, User *exclude) {
    User *current;
    pthread_mutex_lock(&users_mutex);
    current = users;
    pthread_mutex_unlock(&users_mutex);

    while (current) {
        if (current != exclude && current->wsi) {
            if (pthread_mutex_trylock(&current->mutex) == 0) {
                queue_message(current, message, 1);
                pthread_mutex_unlock(&current->mutex);
            }
        }
        current = current->next;
    }
}

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

static cJSON *get_user_list() {
    cJSON *list = cJSON_CreateArray();
    if (!list) {
        fprintf(stderr, "Failed to create user list array\n");
        return NULL;
    }
    pthread_mutex_lock(&users_mutex);
    User *current = users;
    while (current) {
        if (current->username) {
            cJSON *item = cJSON_CreateString(current->username);
            if (!item) {
                fprintf(stderr, "Failed to create username string\n");
                cJSON_Delete(list);
                pthread_mutex_unlock(&users_mutex);
                return NULL;
            }
            cJSON_AddItemToArray(list, item);
        }
        current = current->next;
    }
    pthread_mutex_unlock(&users_mutex);
    return list;
}

static char *get_timestamp() {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char *buf = malloc(20);
    if (!buf) {
        fprintf(stderr, "Failed to allocate timestamp buffer\n");
        return NULL;
    }
    strftime(buf, 20, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

static void send_error(struct lws *wsi, User *user, const char *description) {
    cJSON *error = cJSON_CreateObject();
    if (!error) {
        fprintf(stderr, "Failed to create error object\n");
        return;
    }
    char *timestamp = get_timestamp();
    if (!timestamp) {
        cJSON_Delete(error);
        fprintf(stderr, "Failed to generate timestamp for error\n");
        return;
    }
    cJSON_AddStringToObject(error, "type", "error");
    cJSON_AddStringToObject(error, "sender", "server");
    cJSON_AddStringToObject(error, "content", description);
    cJSON_AddStringToObject(error, "timestamp", timestamp);
    char *json_str = cJSON_PrintUnformatted(error);
    if (!json_str) {
        fprintf(stderr, "Failed to serialize error message\n");
    } else {
        queue_message(user, json_str, 0);
        free(json_str);
    }
    free(timestamp);
    cJSON_Delete(error);
}

static int callback_chat(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user_data, void *in, size_t len) {
    User *user = (User *)user_data;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        user = (User *)user_data;
        if (pthread_mutex_init(&user->mutex, NULL) != 0) {
            fprintf(stderr, "Failed to initialize mutex\n");
            break;
        }
        user->username = NULL;
        user->status = strdup("ACTIVO");
        if (!user->status) {
            fprintf(stderr, "Failed to allocate status\n");
            pthread_mutex_destroy(&user->mutex);
            break;
        }
        user->wsi = wsi;
        user->last_activity_time = time(NULL);
        user->message_queue_head = user->message_queue_tail = NULL;
        user->next = NULL;

        char ip[INET6_ADDRSTRLEN];
        lws_get_peer_simple(wsi, ip, sizeof(ip));
        user->ip_address = strdup(ip);
        if (!user->ip_address) {
            fprintf(stderr, "Failed to allocate ip_address\n");
            free(user->status);
            pthread_mutex_destroy(&user->mutex);
            break;
        }

        pthread_mutex_lock(&users_mutex);
        user->next = users;
        users = user;
        pthread_mutex_unlock(&users_mutex);
        break;
    }

    case LWS_CALLBACK_RECEIVE: {
        if (!user || len == 0) break;
    
        pthread_mutex_lock(&user->mutex);
        user->last_activity_time = time(NULL);
    
        if (strcmp(user->status, "INACTIVO") == 0) {
            free(user->status);
            user->status = strdup("ACTIVO");
            if (!user->status) {
                fprintf(stderr, "Failed to allocate status on reactivation\n");
                pthread_mutex_unlock(&user->mutex);
                break;
            }
            pthread_mutex_unlock(&user->mutex);
    
            cJSON *update = cJSON_CreateObject();
            if (update) {
                cJSON *content = cJSON_CreateObject();
                if (content) {
                    char *timestamp = get_timestamp();
                    if (timestamp) {
                        cJSON_AddStringToObject(update, "type", "status_update");
                        cJSON_AddStringToObject(update, "sender", "server");
                        cJSON_AddStringToObject(content, "user", user->username);
                        cJSON_AddStringToObject(content, "status", "ACTIVO");
                        cJSON_AddItemToObject(update, "content", content);
                        cJSON_AddStringToObject(update, "timestamp", timestamp);
                        char *json_str = cJSON_PrintUnformatted(update);
                        if (json_str) {
                            broadcast_message(json_str, NULL);
                            free(json_str);
                        }
                        free(timestamp);
                    }
                    cJSON_Delete(update);
                } else {
                    cJSON_Delete(update);
                }
            }
            pthread_mutex_lock(&user->mutex);
        }
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
            } else if (!sender || !sender->valuestring || strlen(sender->valuestring) == 0) {
                send_error(wsi, user, "Missing or empty sender for registration");
            } else if (find_user_by_name(sender->valuestring)) {
                send_error(wsi, user, "Username already taken");
            } else {
                pthread_mutex_lock(&user->mutex);
                user->username = strdup(sender->valuestring);
                if (!user->username) {
                    pthread_mutex_unlock(&user->mutex);
                    send_error(wsi, user, "Memory allocation failed");
                    cJSON_Delete(msg);
                    break;
                }
                pthread_mutex_unlock(&user->mutex);

                cJSON *response = cJSON_CreateObject();
                if (!response) {
                    fprintf(stderr, "Failed to create register response\n");
                    cJSON_Delete(msg);
                    break;
                }
                char *timestamp = get_timestamp();
                if (!timestamp) {
                    send_error(wsi, user, "Failed to generate timestamp");
                    cJSON_Delete(response);
                    cJSON_Delete(msg);
                    break;
                }
                cJSON_AddStringToObject(response, "type", "register_success");
                cJSON_AddStringToObject(response, "sender", "server");
                cJSON_AddStringToObject(response, "content", "Registro exitoso");
                cJSON *user_list = get_user_list();
                if (!user_list) {
                    free(timestamp);
                    cJSON_Delete(response);
                    cJSON_Delete(msg);
                    break;
                }
                cJSON_AddItemToObject(response, "userList", user_list);
                cJSON_AddStringToObject(response, "timestamp", timestamp);
                char *json_str = cJSON_PrintUnformatted(response);
                if (!json_str) {
                    send_error(wsi, user, "Failed to serialize response");
                    free(timestamp);
                    cJSON_Delete(response);
                    cJSON_Delete(msg);
                    break;
                }
                queue_message(user, json_str, 0);

                cJSON *connected = cJSON_CreateObject();
                if (!connected) {
                    fprintf(stderr, "Failed to create connected message\n");
                    free(json_str);
                    free(timestamp);
                    cJSON_Delete(response);
                    cJSON_Delete(msg);
                    break;
                }
                cJSON_AddStringToObject(connected, "type", "user_connected");
                cJSON_AddStringToObject(connected, "sender", "server");
                cJSON_AddStringToObject(connected, "content", user->username);
                cJSON_AddStringToObject(connected, "timestamp", timestamp);
                char *connected_str = cJSON_PrintUnformatted(connected);
                if (!connected_str) {
                    send_error(wsi, user, "Failed to serialize connected message");
                } else {
                    broadcast_message(connected_str, user);
                    free(connected_str);
                }

                free(json_str);
                free(timestamp);
                cJSON_Delete(response);
                cJSON_Delete(connected);
            }
        } else {
            if (strcmp(type->valuestring, "broadcast") == 0) {
                const cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (!content || !content->valuestring || strlen(content->valuestring) == 0) {
                    send_error(wsi, user, "Missing or empty broadcast content");
                } else {
                    cJSON *broadcast = cJSON_CreateObject();
                    if (!broadcast) {
                        fprintf(stderr, "Failed to create broadcast object\n");
                        cJSON_Delete(msg);
                        break;
                    }
                    char *timestamp = get_timestamp();
                    if (!timestamp) {
                        send_error(wsi, user, "Failed to generate timestamp");
                        cJSON_Delete(broadcast);
                        cJSON_Delete(msg);
                        break;
                    }
                    cJSON_AddStringToObject(broadcast, "type", "broadcast");
                    cJSON_AddStringToObject(broadcast, "sender", user->username);
                    cJSON_AddStringToObject(broadcast, "content", content->valuestring);
                    cJSON_AddStringToObject(broadcast, "timestamp", timestamp);
                    char *json_str = cJSON_PrintUnformatted(broadcast);
                    if (!json_str) {
                        send_error(wsi, user, "Failed to serialize broadcast");
                    } else {
                        broadcast_message(json_str, NULL);
                        free(json_str);
                    }
                    free(timestamp);
                    cJSON_Delete(broadcast);
                }
            } else if (strcmp(type->valuestring, "private") == 0) {
                const cJSON *target = cJSON_GetObjectItem(msg, "target");
                const cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (!target || !target->valuestring || strlen(target->valuestring) == 0 ||
                    !content || !content->valuestring || strlen(content->valuestring) == 0) {
                    send_error(wsi, user, "Missing or empty target/content for private message");
                } else {
                    User *target_user = find_user_by_name(target->valuestring);
                    if (!target_user) {
                        send_error(wsi, user, "Target user not found");
                    } else {
                        cJSON *private = cJSON_CreateObject();
                        if (!private) {
                            fprintf(stderr, "Failed to create private message object\n");
                            cJSON_Delete(msg);
                            break;
                        }
                        char *timestamp = get_timestamp();
                        if (!timestamp) {
                            send_error(wsi, user, "Failed to generate timestamp");
                            cJSON_Delete(private);
                            cJSON_Delete(msg);
                            break;
                        }
                        cJSON_AddStringToObject(private, "type", "private");
                        cJSON_AddStringToObject(private, "sender", user->username);
                        cJSON_AddStringToObject(private, "target", target->valuestring);
                        cJSON_AddStringToObject(private, "content", content->valuestring);
                        cJSON_AddStringToObject(private, "timestamp", timestamp);
                        char *json_str = cJSON_PrintUnformatted(private);
                        if (!json_str) {
                            send_error(wsi, user, "Failed to serialize private message");
                        } else {
                            queue_message(target_user, json_str, 0);
                            free(json_str);
                        }
                        free(timestamp);
                        cJSON_Delete(private);
                    }
                }
            } else if (strcmp(type->valuestring, "list_users") == 0) {
                cJSON *response = cJSON_CreateObject();
                if (!response) {
                    fprintf(stderr, "Failed to create list_users response\n");
                    cJSON_Delete(msg);
                    break;
                }
                char *timestamp = get_timestamp();
                if (!timestamp) {
                    send_error(wsi, user, "Failed to generate timestamp");
                    cJSON_Delete(response);
                    cJSON_Delete(msg);
                    break;
                }
                cJSON_AddStringToObject(response, "type", "list_users_response");
                cJSON_AddStringToObject(response, "sender", "server");
                cJSON *user_list = get_user_list();
                if (!user_list) {
                    free(timestamp);
                    cJSON_Delete(response);
                    cJSON_Delete(msg);
                    break;
                }
                cJSON_AddItemToObject(response, "content", user_list);
                cJSON_AddStringToObject(response, "timestamp", timestamp);
                char *json_str = cJSON_PrintUnformatted(response);
                if (!json_str) {
                    send_error(wsi, user, "Failed to serialize user list");
                } else {
                    queue_message(user, json_str, 0);
                    free(json_str);
                }
                free(timestamp);
                cJSON_Delete(response);
            } else if (strcmp(type->valuestring, "user_info") == 0) {
                const cJSON *target = cJSON_GetObjectItem(msg, "target");
                if (!target || !target->valuestring || strlen(target->valuestring) == 0) {
                    send_error(wsi, user, "Missing or empty target for user info");
                } else {
                    User *target_user = find_user_by_name(target->valuestring);
                    if (!target_user) {
                        send_error(wsi, user, "User not found");
                    } else {
                        cJSON *response = cJSON_CreateObject();
                        if (!response) {
                            fprintf(stderr, "Failed to create user_info response\n");
                            cJSON_Delete(msg);
                            break;
                        }
                        cJSON *content = cJSON_CreateObject();
                        if (!content) {
                            fprintf(stderr, "Failed to create content for user_info\n");
                            cJSON_Delete(response);
                            cJSON_Delete(msg);
                            break;
                        }
                        char *timestamp = get_timestamp();
                        if (!timestamp) {
                            send_error(wsi, user, "Failed to generate timestamp");
                            cJSON_Delete(response);
                            cJSON_Delete(content);
                            cJSON_Delete(msg);
                            break;
                        }
                        cJSON_AddStringToObject(response, "type", "user_info_response");
                        cJSON_AddStringToObject(response, "sender", "server");
                        cJSON_AddStringToObject(response, "target", target->valuestring);
                        cJSON_AddStringToObject(content, "ip", target_user->ip_address);
                        cJSON_AddStringToObject(content, "status", target_user->status);
                        cJSON_AddItemToObject(response, "content", content);
                        cJSON_AddStringToObject(response, "timestamp", timestamp);
                        char *json_str = cJSON_PrintUnformatted(response);
                        if (!json_str) {
                            send_error(wsi, user, "Failed to serialize user info");
                        } else {
                            queue_message(user, json_str, 0);
                            free(json_str);
                        }
                        free(timestamp);
                        cJSON_Delete(response);
                    }
                }
            } else if (strcmp(type->valuestring, "change_status") == 0) {
                const cJSON *content = cJSON_GetObjectItem(msg, "content");
                if (!content || !content->valuestring || strlen(content->valuestring) == 0) {
                    send_error(wsi, user, "Missing or empty status content");
                } else {
                    const char *new_status = content->valuestring;
                    if (strcmp(new_status, "ACTIVO") != 0 && strcmp(new_status, "OCUPADO") != 0 && strcmp(new_status, "INACTIVO") != 0) {
                        send_error(wsi, user, "Invalid status value");
                    } else {
                        pthread_mutex_lock(&user->mutex);
                        free(user->status);
                        user->status = strdup(new_status);
                        if (!user->status) {
                            pthread_mutex_unlock(&user->mutex);
                            send_error(wsi, user, "Memory allocation failed");
                            cJSON_Delete(msg);
                            break;
                        }
                        pthread_mutex_unlock(&user->mutex);

                        cJSON *update = cJSON_CreateObject();
                        if (!update) {
                            fprintf(stderr, "Failed to create status update object\n");
                            cJSON_Delete(msg);
                            break;
                        }
                        cJSON *content_obj = cJSON_CreateObject();
                        if (!content_obj) {
                            fprintf(stderr, "Failed to create content for status update\n");
                            cJSON_Delete(update);
                            cJSON_Delete(msg);
                            break;
                        }
                        char *timestamp = get_timestamp();
                        if (!timestamp) {
                            send_error(wsi, user, "Failed to generate timestamp");
                            cJSON_Delete(update);
                            cJSON_Delete(content_obj);
                            cJSON_Delete(msg);
                            break;
                        }
                        cJSON_AddStringToObject(update, "type", "status_update");
                        cJSON_AddStringToObject(update, "sender", "server");
                        cJSON_AddStringToObject(content_obj, "user", user->username);
                        cJSON_AddStringToObject(content_obj, "status", new_status);
                        cJSON_AddItemToObject(update, "content", content_obj);
                        cJSON_AddStringToObject(update, "timestamp", timestamp);
                        char *json_str = cJSON_PrintUnformatted(update);
                        if (!json_str) {
                            send_error(wsi, user, "Failed to serialize status update");
                        } else {
                            broadcast_message(json_str, NULL);
                            free(json_str);
                        }
                        free(timestamp);
                        cJSON_Delete(update);
                    }
                }
            } else if (strcmp(type->valuestring, "disconnect") == 0) {
                cJSON *disconnected = cJSON_CreateObject();
                if (!disconnected) {
                    fprintf(stderr, "Failed to create disconnect message\n");
                    cJSON_Delete(msg);
                    break;
                }
                char *timestamp = get_timestamp();
                if (!timestamp) {
                    send_error(wsi, user, "Failed to generate timestamp");
                    cJSON_Delete(disconnected);
                    cJSON_Delete(msg);
                    break;
                }
                cJSON_AddStringToObject(disconnected, "type", "user_disconnected");
                cJSON_AddStringToObject(disconnected, "sender", "server");
                char disconnected_content[256];
                snprintf(disconnected_content, sizeof(disconnected_content), "%s ha salido", user->username);
                cJSON_AddStringToObject(disconnected, "content", disconnected_content);
                cJSON_AddStringToObject(disconnected, "timestamp", timestamp);
                char *json_str = cJSON_PrintUnformatted(disconnected);
                if (!json_str) {
                    send_error(wsi, user, "Failed to serialize disconnect message");
                } else {
                    broadcast_message(json_str, user);
                    free(json_str);
                }
                free(timestamp);
                cJSON_Delete(disconnected);
            } else {
                send_error(wsi, user, "Unknown message type");
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
            size_t len = strlen(msg->data);
            unsigned char *buf = malloc(LWS_PRE + len);
            if (!buf) {
                fprintf(stderr, "Failed to allocate buffer for message\n");
                pthread_mutex_unlock(&user->mutex);
                break;
            }
            memcpy(buf + LWS_PRE, msg->data, len);
            int n = lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
            free(buf);
            free(msg->data);
            free(msg);
            if (n < 0) {
                fprintf(stderr, "Failed to write to wsi for %s\n", user->username ? user->username : "unregistered");
                lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
                pthread_mutex_unlock(&user->mutex);
                break;
            }
            if (user->message_queue_head) {
                if (lws_callback_on_writable(wsi) < 0) {
                    fprintf(stderr, "Failed to request writable callback\n");
                }
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
            if (!disconnected) {
                fprintf(stderr, "Failed to create disconnect message\n");
            } else {
                char *timestamp = get_timestamp();
                if (!timestamp) {
                    fprintf(stderr, "Failed to generate timestamp for disconnect\n");
                    cJSON_Delete(disconnected);
                } else {
                    cJSON_AddStringToObject(disconnected, "type", "user_disconnected");
                    cJSON_AddStringToObject(disconnected, "sender", "server");
                    char disconnected_content[256];
                    snprintf(disconnected_content, sizeof(disconnected_content), "%s ha salido", user->username);
                    cJSON_AddStringToObject(disconnected, "content", disconnected_content);
                    cJSON_AddStringToObject(disconnected, "timestamp", timestamp);
                    char *json_str = cJSON_PrintUnformatted(disconnected);
                    if (!json_str) {
                        fprintf(stderr, "Failed to serialize disconnect message\n");
                    } else {
                        broadcast_message(json_str, user);
                        free(json_str);
                    }
                    free(timestamp);
                    cJSON_Delete(disconnected);
                }
            }
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
        UserCheck *to_check = NULL;
        UserCheck *last = NULL;

        while (current) {
            UserCheck *check = malloc(sizeof(UserCheck));
            if (!check) {
                fprintf(stderr, "Failed to allocate UserCheck in inactivity_check\n");
                while (to_check) {
                    UserCheck *temp = to_check;
                    to_check = to_check->next;
                    free(temp->username);
                    free(temp->status);
                    free(temp);
                }
                pthread_mutex_unlock(&users_mutex);
                continue;
            }
            pthread_mutex_lock(&current->mutex);
            check->username = current->username ? strdup(current->username) : NULL;
            check->status = current->status ? strdup(current->status) : NULL;
            check->wsi = current->wsi;
            check->last_activity_time = current->last_activity_time;
            check->mutex = &current->mutex;
            check->next = NULL;
            pthread_mutex_unlock(&current->mutex);

            if (!check->username || !check->status) {
                fprintf(stderr, "Failed to duplicate username/status for %s\n", 
                        current->username ? current->username : "unregistered");
                free(check->username);
                free(check->status);
                free(check);
            } else {
                if (!to_check) {
                    to_check = last = check;
                } else {
                    last->next = check;
                    last = check;
                }
            }
            current = current->next;
        }
        pthread_mutex_unlock(&users_mutex);

        UserCheck *check = to_check;
        while (check) {
            if (pthread_mutex_trylock(check->mutex) == 0) {
                if (check->username && strcmp(check->status, "INACTIVO") != 0 &&
                    (now - check->last_activity_time) > 30) {
                    User *target_user = find_user_by_name(check->username);
                    if (target_user) {
                        free(target_user->status);
                        target_user->status = strdup("INACTIVO");
                        if (!target_user->status) {
                            fprintf(stderr, "Failed to allocate status in inactivity_check\n");
                            pthread_mutex_unlock(check->mutex);
                            goto next_user;
                        }

                        cJSON *update = cJSON_CreateObject();
                        if (!update) {
                            fprintf(stderr, "Failed to create update object\n");
                            pthread_mutex_unlock(check->mutex);
                            goto next_user;
                        }
                        cJSON *content = cJSON_CreateObject();
                        if (!content) {
                            fprintf(stderr, "Failed to create content object\n");
                            cJSON_Delete(update);
                            pthread_mutex_unlock(check->mutex);
                            goto next_user;
                        }
                        char *timestamp = get_timestamp();
                        if (!timestamp) {
                            fprintf(stderr, "Failed to generate timestamp\n");
                            cJSON_Delete(update);
                            cJSON_Delete(content);
                            pthread_mutex_unlock(check->mutex);
                            goto next_user;
                        }
                        cJSON_AddStringToObject(update, "type", "status_update");
                        cJSON_AddStringToObject(update, "sender", "server");
                        cJSON_AddStringToObject(content, "user", check->username);
                        cJSON_AddStringToObject(content, "status", "INACTIVO");
                        cJSON_AddItemToObject(update, "content", content);
                        cJSON_AddStringToObject(update, "timestamp", timestamp);
                        char *json_str = cJSON_PrintUnformatted(update);
                        if (!json_str) {
                            fprintf(stderr, "Failed to serialize status update\n");
                        } else {
                            broadcast_message(json_str, NULL);
                            free(json_str);
                        }
                        free(timestamp);
                        cJSON_Delete(update);
                    }
                }
                pthread_mutex_unlock(check->mutex);
            }
            next_user:
            UserCheck *temp = check;
            check = check->next;
            free(temp->username);
            free(temp->status);
            free(temp);
        }
    }
    return NULL;
}

static struct lws_protocols protocols[] = {
    {"chat-protocol", callback_chat, sizeof(User), 0}, {NULL, NULL, 0, 0}
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
    if (pthread_create(&inactivity_thread, NULL, inactivity_check, NULL) != 0) {
        fprintf(stderr, "Failed to create inactivity thread\n");
        lws_context_destroy(context);
        return 1;
    }
    pthread_detach(inactivity_thread);

    printf("Server started on port %d\n", port);
    while (1) {
        lws_service(context, 0);
    }

    lws_context_destroy(context);
    return 0;
}
