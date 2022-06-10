#include "asio_server_http1_handler.h"

#include <iostream>

#include "asio_common.h"
#include "asio_server_serve_mux.h"
#include "asio_server_stream.h"
#include "asio_server_request_impl.h"
#include "asio_server_response_impl.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "H2Server_Config_Schema.h"

namespace nghttp2
{

namespace asio_http2
{

namespace server
{

namespace
{
// HTTP request message begin
int http1_msg_begincb(llhttp_t* htp)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    handler->create_stream(++handler->request_count);
    return HPE_OK;
}
} // namespace



namespace
{
// HTTP request message complete
int http1_msg_completecb(llhttp_t* htp)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    auto strm = handler->find_stream(handler->request_count);
    handler->call_on_request(*strm);

    return HPE_OK;
}
} // namespace

namespace
{
int http1_hdr_keycb(llhttp_t* htp, const char* data, size_t len)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    handler->curr_header_name.assign(data, len);
    return HPE_OK;
}
} // namespace

namespace
{
int http1_hdr_valcb(llhttp_t* htp, const char* data, size_t len)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    auto strm = handler->find_stream(handler->request_count);
    if (!strm)
    {
        return HPE_OK;
    }
    auto& req = strm->request().impl();
    req.header().emplace(handler->curr_header_name,
                         header_value{std::string(data, len), true});


    return HPE_OK;
}
} // namespace

namespace
{
int http1_hdrs_completecb(llhttp_t* htp)
{
    return HPE_OK;
}
} // namespace

namespace
{
int http1_body_cb(llhttp_t* htp, const char* data, size_t len)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    auto strm = handler->find_stream(handler->request_count);

    if (!strm)
    {
        return 0;
    }

    strm->request().impl().payload().append((const char*)data, len);

    strm->request().impl().call_on_data((const uint8_t*)data, len);

    return HPE_OK;
}
} // namespace

namespace
{
int http1_on_url_cb(llhttp_t* htp, const char* data, size_t len)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    auto strm = handler->find_stream(handler->request_count);
    if (!strm)
    {
        return HPE_OK;
    }

    auto& req = strm->request().impl();
    auto& uri = req.uri();
    req.method(llhttp_method_name(static_cast<llhttp_method>(htp->method)));

    int rv;
    http_parser_url u{};
    rv = http_parser_parse_url(data, len, 0, &u);
    if (u.field_set & (1 << UF_SCHEMA))
    {
        uri.scheme.assign(util::get_uri_field(data, u, UF_SCHEMA).str());
        if (handler->schema.empty())
        {
            handler->schema = uri.scheme;
        }
    }

    if (u.field_set & (1 << UF_HOST))
    {
        uri.host.assign(util::get_uri_field(data, u, UF_HOST).str());
        if (u.field_set & (1 << UF_PORT))
        {
            uri.host.append(":").append(util::utos(u.port));
        }
        if (handler->host.empty())
        {
            handler->host = uri.host;
        }
    }
    if (u.field_set & (1 << UF_PATH))
    {
        uri.path = util::get_uri_field(data, u, UF_PATH).str();
    }
    else
    {
        uri.path = "/";
    }

    if (u.field_set & (1 << UF_QUERY))
    {
        uri.path += '?';
        uri.path += util::get_uri_field(data, u, UF_QUERY);
    }

    return HPE_OK;
}
} // namespace

namespace
{
int http1_url_complete(llhttp_t* htp)
{
    auto handler = static_cast<http1_handler*>(htp->data);
    handler->should_keep_alive = llhttp_should_keep_alive(htp);

    return HPE_OK;
}
} // namespace


namespace
{
constexpr llhttp_settings_t http1_hooks =
{
    http1_msg_begincb,     // llhttp_cb      on_message_begin;
    http1_on_url_cb,       // llhttp_data_cb on_url;
    nullptr,               // llhttp_data_cb on_status;
    http1_hdr_keycb,       // llhttp_data_cb on_header_field;
    http1_hdr_valcb,       // llhttp_data_cb on_header_value;
    http1_hdrs_completecb, // llhttp_cb      on_headers_complete;
    http1_body_cb,         // llhttp_data_cb on_body;
    http1_msg_completecb,  // llhttp_cb      on_message_complete;
    nullptr,               // llhttp_cb      on_chunk_header
    nullptr,               // llhttp_cb      on_chunk_complete
    nullptr,               // llhttp_cb      on_url_complete
    nullptr,               // llhttp_cb      on_status_complete
    nullptr,               // llhttp_cb      on_header_field_complete
    nullptr                // llhttp_cb      on_header_value_complete
};
} // namespace


http1_handler::http1_handler(boost::asio::io_service& io_service,
                             boost::asio::ip::tcp::endpoint ep,
                             connection_write writefun, serve_mux& mux,
                             const H2Server_Config_Schema& conf)
    : base_handler(io_service, ep, writefun, mux, conf)
{
}

http1_handler::~http1_handler()
{
    for (auto& p : streams_)
    {
        auto& strm = p.second;
        strm->response().impl().call_on_close(NGHTTP2_INTERNAL_ERROR);
    }
}

int http1_handler::start()
{
    llhttp_init(&http_parser, HTTP_REQUEST, &http1_hooks);
    http_parser.data = this;
    return 0;
}

void http1_handler::call_on_request(stream& strm)
{
    auto cb = mux_.handler(strm.request().impl());
    cb(strm.request(), strm.response(), strm.handler()->get_handler_id(), strm.get_stream_id());
}

bool http1_handler::should_stop() const
{
    return (!should_keep_alive);
}

int http1_handler::start_response(stream& strm)
{
    int rv;
    stream_ids_to_respond.push_back(strm.get_stream_id());

    signal_write();
    return 0;
}

int http1_handler::submit_trailer(stream& strm, header_map h)
{

    return 0;
}


void http1_handler::signal_write()
{
    if (!inside_callback_ && !write_signaled_)
    {
        write_signaled_ = true;
        auto self = shared_from_this();
        io_service_.post([self]()
        {
            self->initiate_write();
        });
    }
}

int http1_handler::on_read(const std::vector<uint8_t>& buffer, std::size_t len)
{
    callback_guard cg(*this);

    return 0;
}

int http1_handler::on_write(std::vector<uint8_t>& buffer, std::size_t& len)
{
    callback_guard cg(*this);
    const std::string http10 = "HTTP/1.0";
    const std::string http11 = "HTTP/1.1";
    const std::string crlf = "\r\n";
    const std::string SP = " ";
    const std::string colon = ":";
    const size_t inc_step = 16 * 1024;
    size_t data_len = 0;

    while (stream_ids_to_respond.size())
    {
        auto stream_id = stream_ids_to_respond.front();
        stream_ids_to_respond.pop_front();
        
        auto strm = find_stream(stream_id);
        auto& res = strm->response().impl();
        auto& headers = res.header();
        auto inc_buffer_size = [inc_step](std::vector<uint8_t>& buffer, size_t required_size)
        {
            if (buffer.size() < required_size)
            {
                buffer.resize(buffer.size() + required_size + inc_step);
            }
        };

        auto& http_ver = (should_keep_alive ? http11 : http10);
        auto status_code = std::to_string(res.status_code());
        auto reason_phrase = ::nghttp2::http2::get_reason_phrase(res.status_code());

        size_t least_size = http_ver.size() + SP.size() + status_code.size() + SP.size() + reason_phrase.size() + crlf.size();

        inc_buffer_size(buffer, least_size);

        std::copy_n(http_ver.c_str(), http_ver.size(), std::begin(buffer) + data_len);
        data_len += http_ver.size();

        std::copy_n(SP.c_str(), SP.size(), std::begin(buffer) + data_len);
        data_len += SP.size();

        std::copy_n(status_code.c_str(), status_code.size(), std::begin(buffer) + data_len);
        data_len += status_code.size();

        std::copy_n(SP.c_str(), SP.size(), std::begin(buffer) + data_len);
        data_len += SP.size();

        std::copy_n(reason_phrase.c_str(), reason_phrase.size(), std::begin(buffer) + data_len);
        data_len += reason_phrase.size();

        std::copy_n(crlf.c_str(), crlf.size(), std::begin(buffer) + data_len);
        data_len += crlf.size();
        
        for (auto& header : headers)
        {
            inc_buffer_size(buffer, header.first.size() + colon.size() + header.second.value.size() + crlf.size());
            std::copy_n(header.first.c_str(), header.first.size(), std::begin(buffer) + data_len);
            data_len += header.first.size();

            std::copy_n(colon.c_str(), colon.size(), std::begin(buffer) + data_len);
            data_len += colon.size();

            std::copy_n(header.second.value.c_str(), header.second.value.size(), std::begin(buffer) + data_len);
            data_len += header.second.value.size();

            std::copy_n(crlf.c_str(), crlf.size(), std::begin(buffer) + data_len);
            data_len += crlf.size();

        }

        inc_buffer_size(buffer, crlf.size());
        std::copy_n(crlf.c_str(), crlf.size(), std::begin(buffer) + data_len);
        data_len += crlf.size();


        uint32_t data_flag = 0;
        data_len += res.call_read(&buffer[data_len], buffer.size() - data_len, &data_flag);
        while (!(data_flag & NGHTTP2_DATA_FLAG_EOF))
        {
            inc_buffer_size(buffer, inc_step);
            data_len += res.call_read(&buffer[data_len] + data_len, buffer.size() - data_len, &data_flag);
        }
    }

    len = data_len;
    return 0;
}

void http1_handler::initiate_write()
{
    write_signaled_ = false;
    writefun_();
}

void http1_handler::stream_error(int32_t stream_id, uint32_t error_code)
{
}

void http1_handler::resume(stream& strm)
{
    signal_write();
}

response* http1_handler::push_promise(boost::system::error_code& ec,
                                      stream& strm, std::string method,
                                      std::string raw_path_query,
                                      header_map h)
{
    return nullptr;
}


} // namespace server

} // namespace asio_http2

} // namespace nghttp2

