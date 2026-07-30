#pragma once
#include <string>
#include <stdexcept>
#include "httplib.h"
#include "json.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// PostgRestClient
// Wrapper sobre httplib::Client para hablar con PostgREST v12.
// Cada servicio crea una instancia apuntando a su PostgREST interno.
//
// PostgREST query syntax:
//   Filtro exacto : "id_cita=eq.5"
//   Filtro like   : "nombre=ilike.*juan*"
//   Múltiples     : "estado=eq.ACTIVO&especialidad=eq.Cardiologia"
//
// IMPORTANTE — timeouts: httplib::Client, por defecto, puede esperar
// indefinidamente si PostgREST está lento, sobrecargado o inalcanzable de
// una forma que no rechaza la conexión de inmediato (a diferencia de
// "connection refused", que falla rápido). Sin un timeout explícito, eso se
// traduce en un request que "se queda cargando" para siempre del lado del
// cliente (Postman, JMeter, etc.), sin ningún error ni confirmación. Por
// eso cada llamada configura connect/read/write timeout de forma explícita
// — así, en el peor caso, la llamada falla con un mensaje claro dentro de
// un tiempo acotado, en vez de colgarse.
//
// IMPORTANTE — reutilización de conexión: la versión anterior creaba un
// httplib::Client (y por lo tanto una conexión TCP nueva) en CADA llamada.
// Bajo carga sostenida (por ejemplo, 250 000 inserciones vía JMeter) eso
// generaba cientos de miles de conexiones abiertas y cerradas en pocos
// minutos, acumulando sockets en estado TIME_WAIT más rápido de lo que el
// sistema operativo los libera — hasta agotar los puertos efímeros
// disponibles y producir timeouts intermitentes ("sin respuesta"), aunque
// PostgREST estuviera funcionando bien.
//
// La solución: cada hilo del pool HTTP (BACKEND_THREADS) mantiene su PROPIA
// conexión persistente hacia PostgREST (thread_local), reutilizada entre
// requests sucesivos que ese mismo hilo atiende, con keep-alive habilitado.
// httplib::Client NO es seguro para uso concurrente desde varios hilos a la
// vez — por eso thread_local es la estrategia correcta aquí: cada hilo solo
// toca SU PROPIA instancia, nunca la de otro, así que no hace falta ningún
// mutex. Esto asume una única instancia de PostgRestClient por proceso
// (que es el caso actual del proyecto: se crea una sola vez en main() y se
// comparte vía shared_ptr) — si en el futuro se crea más de una instancia,
// esta estrategia thread_local tendría que revisarse.
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Excepción dedicada para errores de PostgRestClient, en vez de lanzar
// std::runtime_error genérico directamente. Esto permite que el código que
// llama distinga "algo falló hablando con PostgREST" de cualquier otro
// runtime_error del programa, con un catch específico si hace falta.
// ─────────────────────────────────────────────────────────────────────────────
class PostgRestError : public std::runtime_error {
public:
    explicit PostgRestError(const std::string& message) : std::runtime_error(message) {}
};

class PostgRestClient {
public:
    PostgRestClient(const std::string& host, int port,
                     int connectTimeoutSec = 5, int readWriteTimeoutSec = 30)
        : host_(host), port_(port),
          connectTimeoutSec_(connectTimeoutSec), readWriteTimeoutSec_(readWriteTimeoutSec) {}

    // GET /{table}?{query}  →  array JSON
    nlohmann::json getAll(const std::string& table, const std::string& query = "") {
        httplib::Client& cli = clientForThisThread();
        std::string path = "/" + table + (query.empty() ? "" : "?" + query);
        auto res = cli.Get(path, baseHeaders());
        if (!res || res->status != 200) return nlohmann::json::array();
        return nlohmann::json::parse(res->body);
    }

    // GET /{table}?{query}&limit=1  →  objeto JSON o null
    nlohmann::json getOne(const std::string& table, const std::string& query) {
        httplib::Client& cli = clientForThisThread();
        std::string path = "/" + table + "?" + query + "&limit=1";
        auto res = cli.Get(path, baseHeaders());
        if (!res || res->status != 200) return nullptr;
        auto arr = nlohmann::json::parse(res->body);
        if (!arr.is_array() || arr.empty()) return nullptr;
        return arr[0];
    }

    // POST /{table}  →  objeto insertado (con id generado)
    nlohmann::json insert(const std::string& table, const nlohmann::json& data) {
        httplib::Client& cli = clientForThisThread();
        httplib::Headers h = baseHeaders();
        h.emplace("Prefer", "return=representation");
        auto res = cli.Post("/" + table, h, data.dump(), "application/json");
        if (!res || (res->status != 200 && res->status != 201)) {
            // NOSONAR (std::format, S6185): requiere GCC 13+; el Dockerfile
            // usa ubuntu:22.04, que trae GCC 11.4 por defecto — std::format
            // no compilaría. Se mantiene la concatenación manual.
            throw PostgRestError(
                "PostgREST insert error [" + table + "]: " +
                (res ? std::to_string(res->status) + " — " + res->body  // NOSONAR
                     : "sin respuesta (timeout o conexión rechazada tras " +
                           std::to_string(connectTimeoutSec_) + "s)"));  // NOSONAR
        }
        auto result = nlohmann::json::parse(res->body);
        if (result.is_array() && !result.empty()) return result[0];
        return result;
    }

    // POST /{table}  con un arreglo JSON  →  arreglo COMPLETO de objetos
    // insertados (a diferencia de insert(), no se queda solo con el primer
    // elemento). Pensado para los endpoints de lote: recibe N registros ya
    // validados/normalizados y los inserta en una sola llamada HTTP,
    // aprovechando el soporte nativo de PostgREST para bulk insert vía
    // arreglo en el body.
    nlohmann::json insertMany(const std::string& table, const nlohmann::json& dataArray) {
        httplib::Client& cli = clientForThisThread();
        httplib::Headers h = baseHeaders();
        h.emplace("Prefer", "return=representation");
        auto res = cli.Post("/" + table, h, dataArray.dump(), "application/json");
        if (!res || (res->status != 200 && res->status != 201)) {
            // NOSONAR (std::format, S6185): mismo motivo que en insert() —
            // GCC 11.4 (Ubuntu 22.04) no soporta std::format.
            throw PostgRestError(
                "PostgREST bulk insert error [" + table + "]: " +
                (res ? std::to_string(res->status) + " — " + res->body  // NOSONAR
                     : "sin respuesta (timeout o conexión rechazada tras " +
                           std::to_string(connectTimeoutSec_) + "s)"));  // NOSONAR
        }
        return nlohmann::json::parse(res->body);
    }

    // PATCH /{table}?{query}  →  true si OK
    bool update(const std::string& table, const std::string& query, const nlohmann::json& data) {
        httplib::Client& cli = clientForThisThread();
        httplib::Headers h = baseHeaders();
        h.emplace("Prefer", "return=minimal");
        auto res = cli.Patch("/" + table + "?" + query, h, data.dump(), "application/json");
        return res && (res->status == 200 || res->status == 204);
    }

    // DELETE /{table}?{query}  →  true si OK
    bool remove(const std::string& table, const std::string& query) {
        httplib::Client& cli = clientForThisThread();
        auto res = cli.Delete("/" + table + "?" + query, baseHeaders());
        return res && (res->status == 200 || res->status == 204);
    }

private:
    std::string host_;
    int         port_;
    int         connectTimeoutSec_;
    int         readWriteTimeoutSec_;

    httplib::Headers baseHeaders() const {
        return {
            {"Accept",       "application/json"},
            {"Content-Type", "application/json"}
        };
    }

    // Una conexión httplib::Client POR HILO, creada una sola vez y reutilizada
    // en todas las llamadas posteriores que haga ese mismo hilo. Evita abrir
    // una conexión TCP nueva por cada operación contra PostgREST.
    httplib::Client& clientForThisThread() const {
        thread_local httplib::Client cli(host_, port_);
        if (thread_local bool configured = false; !configured) {
            cli.set_connection_timeout(connectTimeoutSec_, 0);
            cli.set_read_timeout(readWriteTimeoutSec_, 0);
            cli.set_write_timeout(readWriteTimeoutSec_, 0);
            cli.set_keep_alive(true);
            configured = true;
        }
        return cli;
    }
};
