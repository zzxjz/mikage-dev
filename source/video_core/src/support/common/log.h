#pragma once

#define LOG_CRITICAL(chan, ...) printf(__VA_ARGS__)
#define LOG_ERROR(...)
#define LOG_TRACE(...)
#define LOG_WARNING(...)

#define ERROR_LOG(...)

#define HW_GPU

#define UNIMPLEMENTED() do { throw std::runtime_error(fmt::format("Unimplemented in file {} at line {}", __FILE__, __LINE__)); } while(0)

#define _dbg_assert_(...)
#define _dbg_assert_msg_(...)
