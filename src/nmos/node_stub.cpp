// SPDX-License-Identifier: MIT
// Builds without nmos-cpp (unit tests, tools): the HTTP/NMOS layer is unavailable.
#include <stdexcept>

#include "nmos/node_api.hpp"

namespace mxlgw::nmosnode
{
    char const* toName(Side side)
    {
        return side == Side::Mxl ? "mxl" : "st2110";
    }

    std::unique_ptr<Node> createNode(Setup, Callbacks)
    {
        throw std::runtime_error("this build has no nmos-cpp support");
    }

    std::unique_ptr<HttpServer> createHttpServer(int, config::Tls const&, ops::Router const&)
    {
        throw std::runtime_error("this build has no nmos-cpp support");
    }
}
