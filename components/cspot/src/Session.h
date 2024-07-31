#pragma once

#include <stdint.h>  // for uint8_t
#include <memory>    // for shared_ptr, unique_ptr
#include <string>    // for string
#include <vector>    // for vector
#include "AuthChallenges.h"

namespace cspot {
class LoginBlob;
class PlainConnection;
class ShannonConnection;
}  // namespace cspot

#define LOGIN_REQUEST_COMMAND 0xAB
#define AUTH_SUCCESSFUL_COMMAND 0xAC
#define AUTH_DECLINED_COMMAND 0xAD

namespace cspot {
class Session {
 protected:
  const LoginBlob& mLoginBlob;
  cspot::AuthChallenges mChallenges;
  std::shared_ptr<cspot::PlainConnection> conn;
  std::string deviceId = "142137fd329622137a14901634264e6f332e2411";

 public:
  Session(const LoginBlob& loginBlob);
  ~Session() {}

  std::shared_ptr<cspot::ShannonConnection> shanConn;

  void connect(std::unique_ptr<cspot::PlainConnection> connection);
  void connectWithRandomAp();
  void close();
  virtual bool triggerTimeout() = 0;
  std::vector<uint8_t> authenticate();
};
}  // namespace cspot
