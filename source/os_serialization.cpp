#define FORMATS_IMPL_EXPLICIT_FORMAT_INSTANTIATIONS_INTENDED
#include <framework/formats_impl.hpp>

#include "os.hpp"

namespace FileFormat {

template struct SerializationInterface<HLE::OS::CodeSetInfo>;

} // namespace FileFormat
