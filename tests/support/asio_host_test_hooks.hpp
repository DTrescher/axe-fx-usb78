#pragma once

// Internal seam for deterministic IASIO lifecycle tests. Production callers
// should use AsioHost::open(), which activates the registered Fractal CLSID.
namespace axe::asio_host_internal {

#if defined(AXE_ASIO_TESTING)
using DriverFactoryForTests = long (*)(void** driver) noexcept;

void setDriverFactoryForTests(DriverFactoryForTests factory) noexcept;
#endif

}  // namespace axe::asio_host_internal
