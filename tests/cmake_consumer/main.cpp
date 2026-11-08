#include <vqf/vqf_core.hpp>

#ifdef VQF_CONSUMER_WITH_NATIVE
#include <vqf/vqf_native.hpp>
#include <vqf/vqf_tiered_native.hpp>
#endif

#include <cstdint>

int main() {
    vqf::VQFEngine baseline(16);
    if (!baseline.insert(42) || !baseline.query(42)) {
        return 1;
    }

#ifdef VQF_CONSUMER_WITH_NATIVE
    if (!vqf::NativeVQF::is_supported()) {
        return 0;
    }
    vqf::NativeVQF native(16);
    if (!native.insert(42) || !native.query(42)) {
        return 2;
    }
    vqf::TieredNativeVQF tiered({16, 88});
    if (!tiered.insert(42) || !tiered.query(42)) {
        return 3;
    }
#endif

    return 0;
}
