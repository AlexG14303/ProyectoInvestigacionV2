#pragma once
#include <string>
#include <functional>
#include <stdexcept>
#include <amqp.h>
#include <amqp_tcp_socket.h>

// ─────────────────────────────────────────────────────────────────────────────
// AmqpConnection
// Wrapper RAII sobre rabbitmq-c (1 conexión, 1 canal).
// Para publisher y consumer se crean instancias separadas.
// ─────────────────────────────────────────────────────────────────────────────
class AmqpConnection {
public:
    AmqpConnection(const std::string& host, int port,
                   const std::string& user, const std::string& password) {
        conn_ = amqp_new_connection();

        amqp_socket_t* socket = amqp_tcp_socket_new(conn_);
        if (!socket)
            throw std::runtime_error("AMQP: no se pudo crear socket TCP");

        if (amqp_socket_open(socket, host.c_str(), port) != AMQP_STATUS_OK)
            throw std::runtime_error("AMQP: no se pudo conectar a " + host + ":" + std::to_string(port));

        amqp_rpc_reply_t login = amqp_login(
            conn_, "/", 0, 131072, 0,
            AMQP_SASL_METHOD_PLAIN, user.c_str(), password.c_str());
        if (login.reply_type != AMQP_RESPONSE_NORMAL)
            throw std::runtime_error("AMQP: login fallido");

        amqp_channel_open(conn_, 1);
        amqp_get_rpc_reply(conn_);
    }

    ~AmqpConnection() {
        amqp_channel_close(conn_, 1, AMQP_REPLY_SUCCESS);
        amqp_connection_close(conn_, AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(conn_);
    }

    // Declarar exchange de tipo "topic" (idempotente)
    void declareExchange(const std::string& exchange, const std::string& type = "topic") {
        amqp_exchange_declare(
            conn_, 1,
            amqp_cstring_bytes(exchange.c_str()),
            amqp_cstring_bytes(type.c_str()),
            0,   // passive
            1,   // durable
            0, 0, amqp_empty_table);
        amqp_get_rpc_reply(conn_);
    }

    // Declarar cola durable (idempotente)
    void declareQueue(const std::string& queue) {
        amqp_queue_declare(
            conn_, 1,
            amqp_cstring_bytes(queue.c_str()),
            0,   // passive
            1,   // durable
            0, 0, amqp_empty_table);
        amqp_get_rpc_reply(conn_);
    }

    // Vincular cola a exchange con routing key (soporta wildcards: citas.*)
    void bindQueue(const std::string& queue,
                   const std::string& exchange,
                   const std::string& routingKey) {
        amqp_queue_bind(
            conn_, 1,
            amqp_cstring_bytes(queue.c_str()),
            amqp_cstring_bytes(exchange.c_str()),
            amqp_cstring_bytes(routingKey.c_str()),
            amqp_empty_table);
        amqp_get_rpc_reply(conn_);
    }

    // Publicar mensaje JSON persistente
    void publish(const std::string& exchange,
                 const std::string& routingKey,
                 const std::string& message) {
        amqp_basic_properties_t props{};
        props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        props.content_type  = amqp_cstring_bytes("application/json");
        props.delivery_mode = 2; // persistente

        amqp_basic_publish(
            conn_, 1,
            amqp_cstring_bytes(exchange.c_str()),
            amqp_cstring_bytes(routingKey.c_str()),
            0, 0, &props,
            amqp_cstring_bytes(message.c_str()));
    }

    // Consumir mensajes de forma bloqueante — ejecutar en std::thread dedicado
    // handler(routingKey, bodyJson)
    void consume(const std::string& queue,
                 std::function<void(const std::string&, const std::string&)> handler) {
        amqp_basic_consume(
            conn_, 1,
            amqp_cstring_bytes(queue.c_str()),
            amqp_empty_bytes,
            0,   // no local
            1,   // no ack (auto-ack)
            0,   // exclusive
            amqp_empty_table);
        amqp_get_rpc_reply(conn_);

        while (true) {
            amqp_rpc_reply_t reply;
            amqp_envelope_t  envelope;
            amqp_maybe_release_buffers(conn_);
            reply = amqp_consume_message(conn_, &envelope, nullptr, 0);
            if (reply.reply_type != AMQP_RESPONSE_NORMAL) break;

            std::string key(static_cast<char*>(envelope.routing_key.bytes),
                            envelope.routing_key.len);
            std::string body(static_cast<char*>(envelope.message.body.bytes),
                             envelope.message.body.len);
            handler(key, body);
            amqp_destroy_envelope(&envelope);
        }
    }

private:
    amqp_connection_state_t conn_;
};
