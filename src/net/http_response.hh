/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef HTTP_RESPONSE_HH
#define HTTP_RESPONSE_HH

#include <memory>

#include "chunked_parser.hh"
#include "http_message.hh"
#include "http_request.hh"

namespace pbrt {

class MIMEType {
  private:
    std::string type_;
    std::vector<std::pair<std::string, std::string>> parameters_;

  public:
    MIMEType(const std::string& content_type);

    const std::string& type() const { return type_; }
};

class HTTPResponse : public HTTPMessage {
  private:
    HTTPRequest request_{};

    /* required methods */
    void calculate_expected_body_size() override;
    size_t read_in_complex_body(const std::string& str) override;
    bool eof_in_body() const override;

    std::unique_ptr<BodyParser> body_parser_{nullptr};

  public:
    void set_request(const HTTPRequest& request);
    const HTTPRequest& request() const { return request_; }

    std::string status_code() const;

    using HTTPMessage::HTTPMessage;
};

}  // namespace pbrt

#endif /* HTTP_RESPONSE_HH */
