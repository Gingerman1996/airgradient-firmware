/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TYPES_HTTP_TYPES_H
#define TYPES_HTTP_TYPES_H

#include <cstdint>
#include <functional>

enum class HttpMethod : uint8_t {
  Get,
  Post,
  Put,
  Delete,
};

enum class HttpStatus : uint16_t {
  Ok = 200,
  Created = 201,
  NoContent = 204,
  Found = 302,
  BadRequest = 400,
  NotFound = 404,
  MethodNotAllowed = 405,
  InternalServerError = 500,
};

class HttpRequest;
struct HttpResponse;

using HttpHandler = std::function<void(const HttpRequest &, HttpResponse &)>;

#endif // TYPES_HTTP_TYPES_H
