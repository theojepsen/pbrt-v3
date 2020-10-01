/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "s3.hh"

#include <fcntl.h>
#include <sys/types.h>

#include <cassert>
#include <chrono>
#include <thread>

#include "awsv4_sig.hh"
#include "http_request.hh"
#include "http_response_parser.hh"
#include "secure_socket.hh"
#include "socket.hh"
#include "util/exception.h"

using namespace std;
using namespace std::chrono;
using namespace pbrt;

const static std::string UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

std::string S3::endpoint(const string& region, const string& bucket) {
    if (region == "us-east-1") {
        return bucket + ".s3.amazonaws.com";
    } else {
        return bucket + ".s3-" + region + ".amazonaws.com";
    }
}

S3GetRequest::S3GetRequest(const AWSCredentials& credentials,
                           const string& endpoint, const string& region,
                           const string& object)
    : AWSRequest(credentials, region, "GET /" + object + " HTTP/1.1", {}) {
    headers_["host"] = endpoint;

    if (credentials.session_token().initialized()) {
        headers_["x-amz-security-token"] = *credentials.session_token();
    }

    AWSv4Sig::sign_request("GET\n/" + object, credentials_.secret_key(),
                           credentials_.access_key(), region_, "s3",
                           request_date_, {}, headers_, {});
}

TCPSocket tcp_connection(const Address& address) {
    TCPSocket sock;
    sock.connect(address);
    return sock;
}

S3Client::S3Client(const AWSCredentials& credentials,
                   const S3ClientConfig& config)
    : credentials_(credentials), config_(config) {}

void S3Client::download_file(const string& bucket, const string& object,
                             string& output) {
    const string endpoint = (config_.endpoint.length() > 0)
                                ? config_.endpoint
                                : S3::endpoint(config_.region, bucket);

    const Address s3_address{endpoint, "https"};

    constexpr milliseconds backoff{50};
    size_t try_count = 0;

    while (true) {
        try_count++;

        if (try_count >= 2) {
            if (try_count >= 7) {
                cerr << "S3Client::download_file: max tries exceeded" << endl;
                throw runtime_error(
                    "S3Client::download_file: max tries exceeded");
            }

            this_thread::sleep_for(backoff * (1 << (try_count - 2)));
        }

        SSLContext ssl_context;
        HTTPResponseParser responses;
        SecureSocket s3 = ssl_context.new_secure_socket( tcp_connection( s3_address ) );
        s3.connect();

        S3GetRequest request{credentials_, endpoint, config_.region, object};
        HTTPRequest outgoing_request = request.to_http_request();
        responses.new_request_arrived(outgoing_request);

        try {
            s3.write(outgoing_request.str());
        } catch (exception& ex) {
            cerr << "S3Client::download: s3.write exception: " << ex.what()
                 << endl;
            continue;
        }

        bool read_error = false;
        while (responses.empty()) {
            try {
                responses.parse(s3.read());
            } catch (exception& ex) {
                cerr << "S3Client::download: s3.read exception: " << ex.what()
                     << endl;
                read_error = true;
                break;
            }
        }

        if (read_error) {
            continue;
        }

        const auto status_code = responses.front().status_code();

        if (status_code == "200") {
            output.swap(responses.front().body());
        } else if (status_code[0] == '4') {
            throw runtime_error("HTTP failure in S3Client::download_file");
        } else {
            try_count++;
            continue;
        }

        return;
    }
}
