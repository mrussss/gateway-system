#pragma once
#include "protocol/Request.hpp"
#include "protocol/Response.hpp"
namespace business
{

    class Dispatcher
    {
    public:
        Response dispatch(const Request &request);
    };

}