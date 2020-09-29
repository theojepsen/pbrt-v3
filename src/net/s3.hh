/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#ifndef S3_HH
#define S3_HH

#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "aws.hh"
#include "http_request.hh"
#include "util/optional.h"
#include "util/path.h"

namespace pbrt {

class S3 {
  public:
    static std::string endpoint(const std::string& region,
                                const std::string& bucket);
};

class S3GetRequest : public AWSRequest {
  public:
    S3GetRequest(const AWSCredentials& credentials, const std::string& endpoint,
                 const std::string& region, const std::string& object);
};

struct S3ClientConfig {
    std::string region{"us-west-1"};
    std::string endpoint{};
    size_t max_threads{32};
    size_t max_batch_size{32};
};

class S3Client {
  private:
    AWSCredentials credentials_;
    S3ClientConfig config_;

  public:
    S3Client(const AWSCredentials& credentials,
             const S3ClientConfig& config = {});

    void download_file(const std::string& bucket, const std::string& object,
                       std::string& output);
};

}  // namespace pbrt

#endif /* S3_HH */
