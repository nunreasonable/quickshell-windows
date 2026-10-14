#include "night_light.hpp"

#include "gamma.hpp"

namespace qs::windows::sys {

void NightLight::enable(int colorTemperatureKelvin) {
	GammaController::instance()->setTemperature(colorTemperatureKelvin);
}

void NightLight::disable() { GammaController::instance()->setTemperature(0); }

} // namespace qs::windows::sys
