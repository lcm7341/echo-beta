#pragma once
#include <cstdint>

namespace echo::objects {
	int* unique_id_counter();
	std::uint64_t* variance_generator();
	void reseed_variance();
}
