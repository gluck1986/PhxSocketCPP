#include "PhxSocket.h"
#include "EasySocket.h"
#include "PhxChannel.h"
#include "ThreadPool.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <thread>

#define POOL_SIZE 1

PhxSocket::PhxSocket(const std::string& url, int interval)
    : pool(POOL_SIZE) {
    this->url = url;
    this->heartBeatInterval = interval;
    this->reconnectOnError = true;
}

PhxSocket::PhxSocket(const std::string& url)
    : PhxSocket(url, 1) {
}

PhxSocket::PhxSocket(
    const std::string& url, int interval, std::shared_ptr<WebSocket> socket)
    : pool(POOL_SIZE) {
    this->url = url;
    this->heartBeatInterval = interval;
    this->reconnectOnError = true;
    this->socket = std::move(socket);
}

void PhxSocket::connect() {
    this->connect(std::map<std::string, std::string>());
}

void PhxSocket::connect(std::map<std::string, std::string> params) {
    std::string url;
    this->params = params;

    // FIXME: Add the parameters to the url.
    if (this->params.size() > 0) {
        url = this->url;
    } else {
        url = this->url;
    }

    this->setCanReconnect(false);

    // The socket hasn't been instantiated with a custom WebSocket.
    if (!this->socket) {
        std::shared_ptr<EasySocket> socket
            = std::make_shared<EasySocket>(url, this);
        this->socket = std::dynamic_pointer_cast<WebSocket, EasySocket>(socket);
    }

    this->socket->setURL(url);
    this->socket->open();
}

void PhxSocket::disconnect() {
    this->discardHeartBeatTimer();
    this->discardReconnectTimer();
    this->disconnectSocket();
}

void PhxSocket::reconnect() {
    this->disconnectSocket();
    this->connect(this->params);
}

void PhxSocket::onOpen(OnOpen callback) {
    this->openCallbacks.push_back(callback);
}

void PhxSocket::onClose(OnClose callback) {
    this->closeCallbacks.push_back(callback);
}

void PhxSocket::onError(OnError callback) {
    this->errorCallbacks.push_back(callback);
}

void PhxSocket::onMessage(OnMessage callback) {
    this->messageCallbacks.push_back(callback);
}

bool PhxSocket::isConnected() {
    return this->socketState() == SocketOpen;
}

void PhxSocket::sendHeartbeat() {
    // clang-format off
    this->push({
            { "topic", "phoenix" },
            { "event", "heartbeat" },
            { "payload", {} },
            { "ref", this->makeRef() }
        });
    // clang-format on
}

int64_t PhxSocket::makeRef() {
    return this->ref++;
}

SocketState PhxSocket::socketState() {
    std::shared_ptr<WebSocket> sk = this->socket;
    if (!sk) {
        return SocketClosed;
    }

    return sk->getSocketState();
}

void PhxSocket::push(nlohmann::json data) {
    this->socket->send(data.dump());
}

// Private

void PhxSocket::discardHeartBeatTimer() {
    this->setCanSendHeartBeat(false);
}

void PhxSocket::discardReconnectTimer() {
    this->setCanReconnect(false);
}

void PhxSocket::disconnectSocket() {
    if (this->socket) {
        this->socket->setDelegate(nullptr);
        this->socket->close();
        this->socket = nullptr;
    }
}

void PhxSocket::onConnOpen() {
    this->discardReconnectTimer();

    // After the socket connection is opened, continue to send heartbeats
    // to keep the connection alive.
    if (this->heartBeatInterval > 0) {
        std::thread thread([this]() {
            this->setCanSendHeartBeat(true);
            while (true) {
                std::this_thread::sleep_for(
                    std::chrono::seconds{ this->heartBeatInterval });

                if (this->canSendHeartbeat) {
                    this->pool.enqueue([this]() { this->sendHeartbeat(); });
                } else {
                    break;
                }
            }
        });

        thread.detach();
    }

    for (int i = 0; i < this->openCallbacks.size(); i++) {
        OnOpen callback = this->openCallbacks.at(i);
        callback();
    }

    if (std::shared_ptr<PhxSocketDelegate> del = this->delegate.lock()) {
        del->phxSocketDidOpen();
    }
}

void PhxSocket::onConnClose(const std::string& event) {
    this->triggerChanError(event);

    // When connection is closed, attempt to reconnect.
    if (this->reconnectOnError) {
        if (!this->reconnecting) {
            this->reconnecting = true;
            this->canReconnect = true;

            std::thread thread([this]() {
                std::this_thread::sleep_for(
                    std::chrono::seconds{ RECONNECT_INTERVAL });

                this->pool.enqueue([this]() {
                    if (this->canReconnect) {
                        this->canReconnect = false;
                        this->reconnect();
                    }

                    this->reconnecting = false;
                });
            });

            thread.detach();
        }
    }

    this->discardHeartBeatTimer();

    for (int i = 0; i < this->closeCallbacks.size(); i++) {
        OnClose callback = this->closeCallbacks.at(i);
        callback(event);
    }

    if (std::shared_ptr<PhxSocketDelegate> del = this->delegate.lock()) {
        del->phxSocketDidClose(event);
    }
}

void PhxSocket::onConnError(const std::string& error) {
    this->discardHeartBeatTimer();

    for (int i = 0; i < this->errorCallbacks.size(); i++) {
        OnError callback = this->errorCallbacks.at(i);
        callback(error);
    }

    if (std::shared_ptr<PhxSocketDelegate> del = this->delegate.lock()) {
        del->phxSocketDidReceiveError(error);
    }

    this->onConnClose(error);
}

void PhxSocket::onConnMessage(const std::string& rawMessage) {
    nlohmann::json json = nlohmann::json::parse(rawMessage);
    const std::string json_topic = json["topic"];
    const std::string json_event = json["event"];
    nlohmann::json json_ref = json["ref"];
    nlohmann::json json_payload = json["payload"];

    // Ref can be null, so check for it first.
    int64_t ref = -1;
    if (!json_ref.is_null()) {
        ref = json_ref;
    }

    for (int i = 0; i < this->channels.size(); i++) {
        std::shared_ptr<PhxChannel> channel = this->channels.at(i);
        if (channel->getTopic() == json_topic) {
            channel->triggerEvent(json_event, json_payload, ref);
        }
    }

    for (int i = 0; i < this->messageCallbacks.size(); i++) {
        OnMessage callback = this->messageCallbacks.at(i);
        callback(json);
    }
}

void PhxSocket::triggerChanError(const std::string& error) {
    for (int i = 0; i < this->channels.size(); i++) {
        std::shared_ptr<PhxChannel> channel = this->channels.at(i);
        channel->triggerEvent("phx_error", error, 0);
    }
}

void PhxSocket::addChannel(std::shared_ptr<PhxChannel> channel) {
    this->channels.emplace_back(channel);
}

void PhxSocket::removeChannel(std::shared_ptr<PhxChannel> channel) {
    std::vector<std::shared_ptr<PhxChannel>> chans = this->channels;
    std::vector<std::shared_ptr<PhxChannel>>::iterator position
        = std::find(chans.begin(), chans.end(), channel);
    if (position != chans.end()) {
        chans.erase(position);
    }
}

void PhxSocket::setDelegate(std::shared_ptr<PhxSocketDelegate> delegate) {
    this->delegate = delegate;
}

void PhxSocket::setCanReconnect(bool canReconnect) {
    this->pool.enqueue(
        [this, canReconnect]() { this->canReconnect = canReconnect; });
}

void PhxSocket::setCanSendHeartBeat(bool canSendHeartbeat) {
    this->pool.enqueue([this, canSendHeartbeat]() {
        this->canSendHeartbeat = canSendHeartbeat;
    });
}

// SocketDelegate

void PhxSocket::webSocketDidOpen(WebSocket* socket) {
    this->pool.enqueue([this]() { this->onConnOpen(); });
}

void PhxSocket::webSocketDidReceive(
    WebSocket* socket, const std::string& message) {
    this->pool.enqueue([this, message]() { this->onConnMessage(message); });
}

void PhxSocket::webSocketDidError(WebSocket* socket, const std::string& error) {
    this->pool.enqueue([this, error]() { this->onConnError(error); });
}

void PhxSocket::webSocketDidClose(
    WebSocket* socket, int code, const std::string& reason, bool wasClean) {
    this->pool.enqueue([this, reason]() { this->onConnClose(reason); });
}

// SocketDelegate
