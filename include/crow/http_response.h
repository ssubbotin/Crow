#pragma once
#include <string>
#include <unordered_map>
#include <ios>
#include <fstream>
#include <sstream>
// S_ISREG is not defined for windows
// This defines it like suggested in https://stackoverflow.com/a/62371749
#if defined(_MSC_VER)
#define _CRT_INTERNAL_NONSTDC_NAMES 1
#endif
#include <sys/stat.h>
#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#endif

#include "crow/http_request.h"
#include "crow/ci_map.h"
#include "crow/socket_adaptors.h"
#include "crow/logging.h"
#include "crow/mime_types.h"
#include "crow/returnable.h"


namespace crow
{
    template<typename Adaptor, typename Handler, typename... Middlewares>
    class Connection;

    class Router;

    /// Outcome of a single chunk provider invocation.
    enum class chunk_result
    {
        more, ///< The chunk is valid and more data is coming.
        done, ///< The chunk is valid and it is the last one; the terminating frame is sent.
        abort ///< The body cannot be finished; the connection is closed without the terminating frame.
    };

    /// HTTP response
    struct response
    {
        template<typename Adaptor, typename Handler, typename... Middlewares>
        friend class crow::Connection;

        friend class Router;

        int code{200};    ///< The Status code for the response.
        std::string body; ///< The actual payload containing the response data.
        ci_map headers;   ///< HTTP headers.

#ifdef CROW_ENABLE_COMPRESSION
        bool compressed = true; ///< If compression is enabled and this is false, the individual response will not be compressed.
#endif
        bool skip_body = false;            ///< Whether this is a response to a HEAD request.
        bool manual_length_header = false; ///< Whether Crow should automatically add a "Content-Length" header.

        /// Provider of the response body, called repeatedly until it returns false.

        ///
        /// The provider fills the given string with the next chunk of the body and returns
        /// `true` while more data is coming, `false` on its last invocation. Leaving the
        /// string empty is allowed and sends no chunk.
        using chunk_provider_t = std::function<bool(std::string&)>;

        /// Outcome of a single chunk provider invocation; see crow::chunk_result.
        using chunk_result = crow::chunk_result;

        /// Provider of the response body, called repeatedly until it returns `done` or `abort`.

        ///
        /// The provider fills the given string with the next chunk of the body and returns
        /// a chunk_result describing how to proceed. Leaving the string empty is allowed
        /// and sends no chunk.
        using chunk_provider_ex_t = std::function<chunk_result(std::string&)>;

        /// Handler called once after the chunked body has been written (or writing has stopped).

        ///
        /// `clean` is `true` when the provider finished with `chunk_result::done` and every
        /// write succeeded, `false` when the provider aborted or a write error occurred.
        using chunk_complete_t = std::function<void(bool clean)>;

        /// Set the value of an existing header in the response.
        void set_header(std::string key, std::string value)
        {
            headers.erase(key);
            headers.emplace(std::move(key), std::move(value));
        }

        /// Add a new header to the response.
        void add_header(std::string key, std::string value)
        {
            headers.emplace(std::move(key), std::move(value));
        }

        const std::string& get_header_value(const std::string& key)
        {
            return crow::get_header_value(headers, key);
        }

        // naive validation of a mime-type string
        static bool validate_mime_type(const std::string& candidate) noexcept
        {
            // Here we simply check that the candidate type starts with
            // a valid parent type, and has at least one character afterwards.
            std::array<std::string, 10> valid_parent_types = {
              "application/", "audio/", "font/", "example/",
              "image/", "message/", "model/", "multipart/",
              "text/", "video/"};
            for (const std::string& parent : valid_parent_types)
            {
                // ensure the candidate is *longer* than the parent,
                // to avoid unnecessary string comparison and to
                // reject zero-length subtypes.
                if (candidate.size() <= parent.size())
                {
                    continue;
                }
                // strncmp is used rather than substr to avoid allocation,
                // but a string_view approach would be better if Crow
                // migrates to C++17.
                if (strncmp(parent.c_str(), candidate.c_str(), parent.size()) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        // Find the mime type from the content type either by lookup,
        // or by the content type itself, if it is a valid a mime type.
        // Defaults to text/plain.
        static std::string get_mime_type(const std::string& contentType)
        {
            const auto mimeTypeIterator = mime_types.find(contentType);
            if (mimeTypeIterator != mime_types.end())
            {
                return mimeTypeIterator->second;
            }
            else if (validate_mime_type(contentType))
            {
                return contentType;
            }
            else
            {
                CROW_LOG_WARNING << "Unable to interpret mime type for content type '" << contentType << "'. Defaulting to text/plain.";
                return "text/plain";
            }
        }


        // clang-format off
        response() {}
        explicit response(int code) : code(code) {}
        response(std::string body) : body(std::move(body)) {}
        response(int code, std::string body) : code(code), body(std::move(body)) {}
        // clang-format on
        response(returnable&& value)
        {
            body = value.dump();
            set_header("Content-Type", value.content_type);
        }
        response(returnable& value)
        {
            body = value.dump();
            set_header("Content-Type", value.content_type);
        }
        response(int code, returnable& value):
          code(code)
        {
            body = value.dump();
            set_header("Content-Type", value.content_type);
        }
        response(int code, returnable&& value):
          code(code), body(value.dump())
        {
            set_header("Content-Type", std::move(value.content_type));
        }

        response(response&& r)
        {
            *this = std::move(r);
        }

        response(std::string contentType, std::string body):
          body(std::move(body))
        {
            set_header("Content-Type", get_mime_type(contentType));
        }

        response(int code, std::string contentType, std::string body):
          code(code), body(std::move(body))
        {
            set_header("Content-Type", get_mime_type(contentType));
        }

        response& operator=(const response& r) = delete;

        response& operator=(response&& r) noexcept
        {
            body = std::move(r.body);
            code = r.code;
            headers = std::move(r.headers);
            completed_ = r.completed_;
            file_info = std::move(r.file_info);
#ifdef CROW_ENABLE_COMPRESSION
            compressed = r.compressed;
#endif
            // skip_body is deliberately not copied: it marks the request side (the router
            // sets it on the connection's response before the handler runs for a HEAD
            // request), so a handler assigning a freshly built response must not reset it.
            manual_length_header = r.manual_length_header;
            chunk_provider_ = std::move(r.chunk_provider_);
            chunk_provider_ex_ = std::move(r.chunk_provider_ex_);
            chunk_complete_ = std::move(r.chunk_complete_);
            return *this;
        }

        /// Check if the response has completed (whether response.end() has been called)
        bool is_completed() const noexcept
        {
            return completed_;
        }

        void clear()
        {
            body.clear();
            code = 200;
            headers.clear();
            completed_ = false;
            file_info = static_file_info{};
            chunk_provider_ = nullptr;
            chunk_provider_ex_ = nullptr;
            chunk_complete_ = nullptr;
        }

        /// Return a "Temporary Redirect" response.

        ///
        /// Location can either be a route or a full URL.
        void redirect(const std::string& location)
        {
            code = 307;
            set_header("Location", location);
        }

        /// Return a "Permanent Redirect" response.

        ///
        /// Location can either be a route or a full URL.
        void redirect_perm(const std::string& location)
        {
            code = 308;
            set_header("Location", location);
        }

        /// Return a "Found (Moved Temporarily)" response.

        ///
        /// Location can either be a route or a full URL.
        void moved(const std::string& location)
        {
            code = 302;
            set_header("Location", location);
        }

        /// Return a "Moved Permanently" response.

        ///
        /// Location can either be a route or a full URL.
        void moved_perm(const std::string& location)
        {
            code = 301;
            set_header("Location", location);
        }

        void write(const std::string& body_part)
        {
            body += body_part;
        }

        /// Set the response completion flag and call the handler (to send the response).
        void end()
        {
            if (!completed_)
            {
                completed_ = true;
                if (skip_body)
                {
                    if (is_chunked_type())
                    {
                        // A response to HEAD must carry the same header fields a GET would
                        // produce; with a chunk provider the body length is unknown, so
                        // "Transfer-Encoding: chunked" is kept and "Content-Length" is not
                        // set (RFC 7230 forbids sending both at once). The body itself is
                        // skipped, so the provider is dropped without being called. The
                        // completion handler is still invoked (with clean == true) so that
                        // it remains the single release point for the data source no matter
                        // which method the client used.
                        chunk_provider_ = nullptr;
                        chunk_provider_ex_ = nullptr;
                        body = "";
                        manual_length_header = true;
                        if (chunk_complete_)
                        {
                            auto completion_handler = std::move(chunk_complete_);
                            chunk_complete_ = nullptr;
                            try
                            {
                                completion_handler(true);
                            }
                            catch (...)
                            {
                                CROW_LOG_ERROR << "An uncaught exception occurred in the chunked completion handler.";
                            }
                        }
                    }
                    else
                    {
                        set_header("Content-Length", std::to_string(body.size()));
                        body = "";
                        manual_length_header = true;
                    }
                }
                if (complete_request_handler_)
                {
                    complete_request_handler_();
                    manual_length_header = false;
                    skip_body = false;
                }
            }
        }

        /// Same as end() except it adds a body part right before ending.
        void end(const std::string& body_part)
        {
            body += body_part;
            end();
        }

        /// Check if the connection is still alive (usually by checking the socket status).
        bool is_alive()
        {
            return is_alive_helper_ && is_alive_helper_();
        }

        /// Check whether the response has a static file defined.
        bool is_static_type()
        {
            return file_info.path.size();
        }

        /// Check whether the response body is produced by a chunk provider.
        bool is_chunked_type() const
        {
            return static_cast<bool>(chunk_provider_) || static_cast<bool>(chunk_provider_ex_);
        }

        /// Send the response body in chunks produced on demand, without holding it in memory.

        ///
        /// The body is sent using `Transfer-Encoding: chunked`, so its size need not be known
        /// in advance, which makes it suitable for bodies of arbitrary or unknown length. The
        /// provider runs on the connection thread while the response is being written. The
        /// provider should not throw: an exception that escapes it is logged and treated as
        /// an abort (the connection is closed without the terminating frame).
        void set_chunked_content_provider(chunk_provider_t provider, std::string content_type = "")
        {
            set_chunked_content_provider(
              [provider = std::move(provider)](std::string& chunk) {
                  return provider(chunk) ? chunk_result::more : chunk_result::done;
              },
              std::move(content_type));
        }

        /// Send the response body in chunks produced on demand, without holding it in memory.

        ///
        /// Same as the `chunk_provider_t` overload, except that the provider can also return
        /// `chunk_result::abort` to close the connection without the terminating frame, so
        /// that the client sees a truncated body instead of a seemingly complete one.
        /// Any previously set "Content-Length" header is removed: chunked transfer encoding
        /// and "Content-Length" must not be sent together.
        void set_chunked_content_provider(chunk_provider_ex_t provider, std::string content_type = "")
        {
            chunk_provider_ex_ = std::move(provider);
            manual_length_header = true;
            headers.erase("Content-Length");
            set_header("Transfer-Encoding", "chunked");
            if (!content_type.empty())
            {
                set_header("Content-Type", std::move(content_type));
            }
        }

        /// Set a handler called once after the chunked body has been written (or writing has stopped).

        ///
        /// The handler runs on the connection thread before the response is finalized. Its
        /// `clean` argument is `true` when the provider finished normally (`chunk_result::done`,
        /// or `false` from the `chunk_provider_t` overload) and every write succeeded. For a
        /// HEAD request the body is skipped and the provider is never called, but the handler
        /// still runs (with `clean == true`) when the response ends, so it remains a reliable
        /// place to release the source of the data. The handler should not throw: an exception
        /// that escapes it is logged and swallowed.
        void set_chunked_completion_handler(chunk_complete_t handler)
        {
            chunk_complete_ = std::move(handler);
        }

        /// This constains metadata (coming from the `stat` command) related to any static files associated with this response.

        ///
        /// Either a static file or a string body can be returned as 1 response.
        struct static_file_info
        {
            std::string path = "";
            struct stat statbuf;
            int statResult;
        };

        /// Return a static file as the response body
        void set_static_file_info(std::string path)
        {
            utility::sanitize_filename(path);
            set_static_file_info_unsafe(path);
        }

        /// Return a static file as the response body without sanitizing the path (use set_static_file_info instead)
        void set_static_file_info_unsafe(std::string path)
        {
            file_info.path = path;
            file_info.statResult = stat(file_info.path.c_str(), &file_info.statbuf);
#ifdef CROW_ENABLE_COMPRESSION
            compressed = false;
#endif
            if (file_info.statResult == 0 && S_ISREG(file_info.statbuf.st_mode))
            {
                std::size_t last_dot = path.find_last_of(".");
                std::string extension = path.substr(last_dot + 1);
                code = 200;
                this->add_header("Content-Length", std::to_string(file_info.statbuf.st_size));

                if (!extension.empty())
                {
                    this->add_header("Content-Type", get_mime_type(extension));
                }
            }
            else
            {
                code = 404;
                file_info.path.clear();
            }
        }

    private:
        bool completed_{};
        std::function<void()> complete_request_handler_;
        std::function<bool()> is_alive_helper_;
        static_file_info file_info;
        chunk_provider_t chunk_provider_;
        chunk_provider_ex_t chunk_provider_ex_;
        chunk_complete_t chunk_complete_;
    };
} // namespace crow
