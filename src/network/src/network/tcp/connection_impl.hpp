#ifndef NEXUSMINER_NETWORK_TCP_CONNECTION_IMPL_HPP
#define NEXUSMINER_NETWORK_TCP_CONNECTION_IMPL_HPP

#include "asio/io_service.hpp"
#include "asio/write.hpp"
#include "network/connection.hpp"
#include "network/tcp/protocol_description.hpp"
#include <queue>
#include <memory>

namespace nexusminer {
namespace network {
namespace tcp {

template<typename ProtocolDescriptionType>
class Connection_impl
    : public Connection
    , public std::enable_shared_from_this<Connection_impl<ProtocolDescriptionType>>
{
    // protocol specific types
    using Protocol_description = ProtocolDescriptionType;
    using Protocol_socket = typename Protocol_description::Socket;
    using Protocol_endpoint = typename Protocol_description::Endpoint;

public:
    Connection_impl(std::shared_ptr<::asio::io_context> io_context,
                    Endpoint remote_endpoint, Endpoint local_endpoint, Connection::Handler handler);
    Connection_impl(std::shared_ptr<::asio::io_context> io_context,
                    std::shared_ptr<Protocol_socket> asio_socket, Endpoint remote_endpoint);

    // no copies
    Connection_impl(const Connection_impl&) = delete;
    Connection_impl& operator=(const Connection_impl&) = delete;
    // no moves
    Connection_impl(Connection_impl&&) = delete;
    Connection_impl& operator=(Connection_impl&&) = delete;

    // Connection interface
    Endpoint const& remote_endpoint() const override { return m_remote_endpoint; }
    Endpoint const& local_endpoint() const override { return m_local_endpoint; }
    void transmit(Shared_payload tx_buffer) override;
    void close() override;

    // interface towards socket
    Result::Code connect();
    void handle_accept(Connection::Handler connection_handler);

private:
    std::weak_ptr<Connection_impl<ProtocolDescriptionType>> get_weak_self();
    Result::Code initialise_socket();
    void transmit_trigger();
    void receive();
    void change(Result::Code code);
    void close_internal(Result::Code code);

    std::shared_ptr<::asio::io_context> m_io_context;
    std::shared_ptr<Protocol_socket> m_asio_socket;
    Endpoint m_remote_endpoint;
    Endpoint m_local_endpoint;
    std::queue<Shared_payload> m_tx_queue;
    Connection::Handler m_connection_handler;
};


template<typename ProtocolDescriptionType>
inline Connection_impl<ProtocolDescriptionType>::Connection_impl(
    std::shared_ptr<::asio::io_context> io_context, Endpoint remote_endpoint,
    Endpoint local_endpoint, Connection::Handler handler)
    : m_io_context{std::move(io_context)}
    , m_asio_socket{std::make_shared<Protocol_socket>(*m_io_context)}
    , m_remote_endpoint{std::move(remote_endpoint)}
    , m_local_endpoint{std::move(local_endpoint)}
    , m_tx_queue{}
    , m_connection_handler{std::move(handler)}
{
}

template<typename ProtocolDescriptionType>
inline Connection_impl<ProtocolDescriptionType>::Connection_impl(
    std::shared_ptr<::asio::io_context> io_context,
    std::shared_ptr<Protocol_socket> asio_socket, Endpoint remote_endpoint)
    : m_io_context{std::move(io_context)}
    , m_asio_socket{std::move(asio_socket)}
    , m_remote_endpoint{std::move(remote_endpoint)}
    , m_local_endpoint{}     // will be set later, this constructor is called in accept/listen case
    , m_tx_queue{}
	, m_connection_handler{} // will be set later, this constructor is called in accept/listen case
{
}

template<typename ProtocolDescriptionType>
inline std::weak_ptr<Connection_impl<ProtocolDescriptionType>>
Connection_impl<ProtocolDescriptionType>::get_weak_self()
{
    return this->shared_from_this();
}

template<typename ProtocolDescriptionType>
inline Result::Code
Connection_impl<ProtocolDescriptionType>::initialise_socket()
{
    asio::error_code error;
    this->m_asio_socket->open(get_endpoint_base<Protocol_endpoint>(m_local_endpoint).protocol(), error);
    if (error) 
	{
        return Result::error;
    }

    this->m_asio_socket->bind(get_endpoint_base<Protocol_endpoint>(m_local_endpoint), error);
    if (error)
	{
        m_asio_socket->close(error);
        return Result::error;
    }

    Protocol_description::update_port(m_asio_socket->local_endpoint(), m_local_endpoint);

    return Result::ok;
}


template<typename ProtocolDescriptionType>
inline Result::Code Connection_impl<ProtocolDescriptionType>::connect()
{
    if (initialise_socket() != Result::ok)
	{
        return Result::error;
    }

    std::weak_ptr<Connection_impl<ProtocolDescriptionType>> weak_self = this->shared_from_this();
    this->m_asio_socket->async_connect(get_endpoint_base<Protocol_endpoint>(m_remote_endpoint),
                                       [weak_self](::asio::error_code const& error)
	{
		auto self = weak_self.lock();
		if (self && self->m_connection_handler)
		{
            if (!error) 
            {
                self->change(Result::Code::connection_ok);
            }
            else 
            {
                self->change(Result::Code::connection_declined);
            }
		}
	});

    return Result::ok;
}

template<typename ProtocolDescriptionType>
inline void Connection_impl<ProtocolDescriptionType>::receive()
{
    m_asio_socket->async_receive(asio::null_buffers(), [weak_self = get_weak_self()](auto error, auto) 
	{
        auto self = weak_self.lock();
        if (self && self->m_connection_handler) 
		{
            if (!error) 
            {
                // read length of received message;
                auto const length = self->m_asio_socket->available();

                if (length == 0)
                {
                    self->change(Result::Code::connection_closed);
                    return;
                }

                Shared_payload receive_buffer = std::make_shared<std::vector<std::uint8_t>>(length);
                receive_buffer->resize(length);

                self->m_asio_socket->receive(asio::buffer(*receive_buffer, receive_buffer->size()), 0, error);
                if (!error)
                {
                    self->m_connection_handler(Result::receive_ok, std::move(receive_buffer));
                    self->receive();
                }
                else
                {
                    // established connection fails for any other reason
                    self->change(Result::Code::connection_aborted);
                }
            }
            else if ((error == ::asio::error::eof) || (error == ::asio::error::connection_reset))
            {
                // established connection closed by remote
                self->change(Result::Code::connection_closed);
            }
            else
            {
                // established connection fails for any other reason
                self->change(Result::Code::connection_aborted);
            }
        }
    });
}

template<typename ProtocolDescriptionType>
inline void Connection_impl<ProtocolDescriptionType>::change(Result::Code code)
{
    if (code == Result::Code::connection_ok) 
    {
        m_connection_handler(code, Shared_payload{});
        receive();
    }
    else 
    {
        close_internal(code);
    }
}

template<typename ProtocolDescriptionType>
inline void Connection_impl<ProtocolDescriptionType>::handle_accept(Connection::Handler connection_handler)
{
    assert(connection_handler);
    m_connection_handler = std::move(connection_handler);
    m_local_endpoint = Endpoint(m_asio_socket->local_endpoint());
    change(Result::Code::connection_ok);
}


template<typename ProtocolDescriptionType>
void Connection_impl<ProtocolDescriptionType>::transmit(Shared_payload tx_buffer)
{
    // only for non closed connection
    if (m_connection_handler) 
    {
        m_tx_queue.emplace(tx_buffer);
        if (m_tx_queue.size() == 1) 
        {
            transmit_trigger();
        }
    }
}

template<typename ProtocolDescriptionType>
void Connection_impl<ProtocolDescriptionType>::transmit_trigger()
{
    auto const payload = m_tx_queue.front();
    ::asio::async_write(*m_asio_socket, ::asio::buffer(*payload, payload->size()),
        // don't forget to keep the payload until transmission has been completed!!!
        [weak_self = get_weak_self(), payload](auto, auto) 
        {
            auto self = weak_self.lock();
            if ((self != nullptr) && self->m_connection_handler) 
            {
                self->m_tx_queue.pop();
                if (!self->m_tx_queue.empty()) 
                {
                    self->transmit_trigger();
                }
            }
        });
}

template<typename ProtocolDescriptionType>
inline void Connection_impl<ProtocolDescriptionType>::close()
{
    close_internal(Result::Code::connection_closed);
}

template<typename ProtocolDescriptionType>
inline void Connection_impl<ProtocolDescriptionType>::close_internal(Result::Code code)
{
    if (m_connection_handler) 
    {
        if (m_asio_socket->is_open()) 
        {
            ::asio::error_code error;
            (void)m_asio_socket->shutdown(::asio::socket_base::shutdown_both, error);
            (void)m_asio_socket->close(error);
        }

        auto const connection_handler = std::move(m_connection_handler);
        m_connection_handler = nullptr;
        connection_handler(code, Shared_payload{});
    }
}

}
}
}

#endif