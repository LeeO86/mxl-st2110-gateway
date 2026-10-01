// SPDX-License-Identifier: MIT
// Setup mode (§9.1): a bare nmos::server with only the gateway routes on node.http_port.
#include "cpprest/api_router.h"
#include "nmos/api_utils.h"
#include "nmos/certificate_handlers.h"
#include "nmos/model.h"
#include "nmos/server.h"
#include "nmos/server_utils.h"
#include "nmos/settings.h"

#include "nmos/http_adapter.hpp"
#include "nmos/node_api.hpp"
#include "util/logging.hpp"

namespace mxlgw::nmosnode
{
    namespace
    {
        class BareServer final : public HttpServer
        {
        public:
            BareServer(int port, config::Tls const& tls, ops::Router const& routes)
                : _port(port)
                , _routes(routes)
            {
                web::json::value s = web::json::value::object();
                s[U("http_port")] = port;
                if (tls.enabled)
                {
                    s[U("server_secure")] = true;
                    s[U("server_certificates")] =
                        web::json::value_of({web::json::value_of({{U("key_algorithm"), U("ECDSA")},
                                                                  {U("private_key_file"), web::json::value::string(tls.privateKey)},
                                                                  {U("certificate_chain_file"), web::json::value::string(tls.certificate)}})});
                }
                _model.settings = s;
                nmos::insert_node_default_settings(_model.settings);
                _server = std::make_unique<nmos::server>(_model);
                auto& api = _server->api_routers[{{}, port}];
                mountGatewayRoutes(api, _routes, _model.settings, nmosGate(), false);
                auto const config = nmos::make_http_listener_config(_model.settings, nmos::make_load_server_certificates_handler(_model.settings, nmosGate()),
                                                                    nmos::make_load_dh_param_handler(_model.settings, nmosGate()), {}, nmosGate());
                // make_api_listener -> support_api adds the finally handler and CORS support.
                _server->http_listeners.push_back(nmos::make_api_listener(tls.enabled, web::http::experimental::listener::host_wildcard, port, api, config,
                                                                          nmos::experimental::get_hsts(_model.settings), nmosGate()));
            }

            ~BareServer() override { stop(); }

            void start() override
            {
                _server->open().wait();
                _open = true;
                log::info("http_server_started", {{"port", _port}});
            }

            void stop() override
            {
                if (_open)
                {
                    _server->close().wait();
                    _open = false;
                }
            }

        private:
            int _port;
            ops::Router const& _routes;
            nmos::node_model _model;
            std::unique_ptr<nmos::server> _server;
            bool _open = false;
        };
    }

    std::unique_ptr<HttpServer> createHttpServer(int port, config::Tls const& tls, ops::Router const& routes)
    {
        return std::make_unique<BareServer>(port, tls, routes);
    }
}
