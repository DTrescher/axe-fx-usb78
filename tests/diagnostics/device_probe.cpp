#include "windows_audio.hpp"
#include <iostream>
int main() {
    try {
        for(const auto& e:axe::win::enumerate_render()) {
            std::cout << (e.is_default?"* ":"  ") << axe::win::utf8(e.name) << "\n  "
                << axe::win::utf8(e.id) << "\n  " << e.rate << "Hz " << e.channels << "ch " << e.bits
                << "bit " << (e.ieee_float?"float":"PCM") << "; engine=" << e.period_frames
                << " frames, min=" << e.minimum_frames << " frames\n";
        }
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
