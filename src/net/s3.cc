/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "s3.hh"

#include <cassert>
#include <thread>
#include <fcntl.h>
#include <sys/types.h>

#include "socket.hh"
#include "secure_socket.hh"
#include "http_request.hh"
#include "http_response_parser.hh"
#include "awsv4_sig.hh"
#include "util/exception.h"

using namespace std;
using namespace pbrt;

const static std::string UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

std::string S3::endpoint( const string & region, const string & bucket )
{
  if ( region == "us-east-1" ) {
    return bucket + ".s3.amazonaws.com";
  }
  else {
    return bucket + ".s3-" + region + ".amazonaws.com";
  }
}

S3GetRequest::S3GetRequest( const AWSCredentials & credentials,
                            const string & endpoint, const string & region,
                            const string & object )
  : AWSRequest( credentials, region, "GET /" + object + " HTTP/1.1", {} )
{
  headers_[ "host" ] = endpoint;

  if ( credentials.session_token().initialized() ) {
    headers_[ "x-amz-security-token" ] = *credentials.session_token();
  }

  AWSv4Sig::sign_request( "GET\n/" + object,
                          credentials_.secret_key(), credentials_.access_key(),
                          region_, "s3", request_date_, {}, headers_,
                          {} );
}

TCPSocket tcp_connection( const Address & address )
{
  TCPSocket sock;
  sock.connect( address );
  return sock;
}

S3Client::S3Client( const AWSCredentials & credentials,
                    const S3ClientConfig & config )
  : credentials_( credentials ), config_( config )
{}

void S3Client::download_file( const string & bucket, const string & object,
                              const roost::path & filename )
{
  const string endpoint = ( config_.endpoint.length() > 0 )
                          ? config_.endpoint : S3::endpoint( config_.region, bucket );
  const Address s3_address { endpoint, "https" };

  SSLContext ssl_context;
  HTTPResponseParser responses;
  SecureSocket s3 = ssl_context.new_secure_socket( tcp_connection( s3_address ) );
  s3.connect();

  S3GetRequest request { credentials_, endpoint, config_.region, object };
  HTTPRequest outgoing_request = request.to_http_request();
  responses.new_request_arrived( outgoing_request );
  s3.write( outgoing_request.str() );

  FileDescriptor file { CheckSystemCall( "open",
    open( filename.string().c_str(), O_RDWR | O_TRUNC | O_CREAT,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH ) ) };

  while ( responses.empty() ) {
    responses.parse( s3.read() );
  }

  if ( responses.front().first_line() != "HTTP/1.1 200 OK" ) {
    throw runtime_error( "HTTP failure in S3Client::download_file( " + bucket + ", " + object + " ): " + responses.front().first_line() );
  }
  else {
    file.write( responses.front().body(), true );
  }
}
