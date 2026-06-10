#pragma once
#include "control/ControlPlaneClient.hpp"
#include "protocol/Request.hpp"
#include "protocol/Response.hpp"
namespace business
{

    class Dispatcher
    {
    public:
        explicit Dispatcher(const ControlPlaneClient &control_plane);
        Response dispatch(const Request &request);

    private:
        const ControlPlaneClient &control_plane_;
    };

}
