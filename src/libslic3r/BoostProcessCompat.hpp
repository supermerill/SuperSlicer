// boost_process_compat.hpp - Fixed version for namespace issues
#pragma once

#include <boost/version.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <array>

#include <stdexcept>

// Debug: Print what we detect
#pragma message("Boost version: " BOOST_PP_STRINGIZE(BOOST_VERSION))

// Detect available Boost.Process APIs based on version
#if BOOST_VERSION >= 108500
    // V2 was added in Boost 1.85
    #define BOOST_PROCESS_V2_AVAILABLE
    #define BOOST_PROCESS_V1_AVAILABLE  // V1 is still available alongside V2
    #pragma message("V2 API available (Boost >= 1.85)")
#elif BOOST_VERSION >= 106400
    // V1 only for Boost 1.64 - 1.84
    #define BOOST_PROCESS_V1_AVAILABLE
    #pragma message("V1 API available (Boost 1.64-1.84)")
#else
    #pragma message("Boost version too old for process library")
#endif

// Prefer V2 for modern features, fallback to V1 for compatibility
#ifndef FORCE_BOOST_PROCESS_V1
    #ifdef BOOST_PROCESS_V2_AVAILABLE
        #define USE_BOOST_PROCESS_V2
        #pragma message("Using V2 API")
    #elif defined(BOOST_PROCESS_V1_AVAILABLE)
        #define USE_BOOST_PROCESS_V1
        #pragma message("Using V1 API")
    #endif
#else
    #ifdef BOOST_PROCESS_V1_AVAILABLE
        #define USE_BOOST_PROCESS_V1
        #pragma message("Forced V1 API")
    #endif
#endif

// Include appropriate headers
#ifdef USE_BOOST_PROCESS_V1
    #include <boost/process.hpp>
    #include <boost/process/v1/child.hpp>
    #include <boost/process/v1/pipe.hpp>
    #include <boost/process/v1/io.hpp>
    #include <boost/process/v1/args.hpp>
    #if __has_include(<boost/process/v1/search_path.hpp>)
        #include <boost/process/v1/search_path.hpp>
        #pragma message("Including V1 search_path header")
    #elif __has_include(<boost/process/search_path.hpp>)
        #include <boost/process/search_path.hpp>
        #pragma message("Including legacy search_path header")
    #endif
#elif defined(USE_BOOST_PROCESS_V2)
    #include <boost/process/v2/process.hpp>
    #include <boost/process/v2/environment.hpp>
    #include <boost/process/v2/stdio.hpp>
    #include <boost/asio.hpp>
    #pragma message("Including V2 headers")
#else
    #error "No compatible Boost.Process version found"
#endif

namespace process_compat {

    // ProcessWrapper class to provide unified interface
    class ProcessWrapper {
    private:
#ifdef USE_BOOST_PROCESS_V2
        boost::asio::io_context ctx_;
        boost::asio::readable_pipe stdout_pipe_;
        boost::asio::readable_pipe stderr_pipe_;
        boost::asio::writable_pipe stdin_pipe_;
        std::unique_ptr<boost::process::v2::process> process_;
        std::string stdout_buffer_;
        std::string stderr_buffer_;
#elif defined(USE_BOOST_PROCESS_V1)
        std::unique_ptr<boost::process::v1::child> process_;
        std::unique_ptr<boost::process::v1::opstream> stdin_stream_;
        std::unique_ptr<boost::process::v1::ipstream> stdout_stream_;
        std::unique_ptr<boost::process::v1::ipstream> stderr_stream_;
#endif

    public:
        ProcessWrapper() 
#ifdef USE_BOOST_PROCESS_V2
            : stdout_pipe_(ctx_), stderr_pipe_(ctx_), stdin_pipe_(ctx_)
#endif
        {}

        ~ProcessWrapper() {
            if (process_) {
                try {
                    wait();
                } catch (...) {}
            }
        }

		// In the ProcessWrapper start() method, improve error reporting:
		void start(const std::string& exe, const std::vector<std::string>& args) {
#ifdef USE_BOOST_PROCESS_V2
			try {
				process_ = std::make_unique<boost::process::v2::process>(
					ctx_, exe, args,
					boost::process::v2::process_stdio{stdin_pipe_, stdout_pipe_, stderr_pipe_}
				);
			} catch (const std::exception& e) {
				throw std::runtime_error("Failed to start process with V2 API: " + std::string(e.what()));
			}
#elif defined(USE_BOOST_PROCESS_V1)
			stdin_stream_ = std::make_unique<boost::process::v1::opstream>();
			stdout_stream_ = std::make_unique<boost::process::v1::ipstream>();
			stderr_stream_ = std::make_unique<boost::process::v1::ipstream>();
			
			try {
				// Try V1 namespace first
				process_ = std::make_unique<boost::process::v1::child>(
					exe, boost::process::v1::args(args),
					boost::process::v1::std_in < *stdin_stream_,
					boost::process::v1::std_out > *stdout_stream_,
					boost::process::v1::std_err > *stderr_stream_
				);
			} catch (const std::exception&) {
				// Fallback to main namespace for older Boost versions
				try {
					using namespace boost::process;
					process_ = std::make_unique<child>(
						exe, args(args),
						std_in < *stdin_stream_,
						std_out > *stdout_stream_,
						std_err > *stderr_stream_
					);
				} catch (const std::exception& e) {
					throw std::runtime_error("Failed to start process with V1 API: " + std::string(e.what()));
				}
			}
#endif
		}

        void wait() {
            if (process_) {
                process_->wait();
            }
        }

        bool running() const {
            return process_ && process_->running();
        }

        void run_io() {
#ifdef USE_BOOST_PROCESS_V2
            // Read available data from pipes
            boost::system::error_code ec;
            std::array<char, 1024> buffer;
            
            auto bytes_read = stdout_pipe_.read_some(boost::asio::buffer(buffer), ec);
            if (!ec && bytes_read > 0) {
                stdout_buffer_.append(buffer.data(), bytes_read);
            }
            
            bytes_read = stderr_pipe_.read_some(boost::asio::buffer(buffer), ec);
            if (!ec && bytes_read > 0) {
                stderr_buffer_.append(buffer.data(), bytes_read);
            }
#elif defined(USE_BOOST_PROCESS_V1)
            // For V1, this is handled automatically by the streams
#endif
        }

        // Stream-like interface for compatibility
        class OutputStream {
            ProcessWrapper* parent_;
        public:
            OutputStream(ProcessWrapper* p) : parent_(p) {}
            void write(const std::string& data) {
#ifdef USE_BOOST_PROCESS_V2
                boost::asio::write(parent_->stdin_pipe_, boost::asio::buffer(data));
#elif defined(USE_BOOST_PROCESS_V1)
                if (parent_->stdin_stream_) {
                    *parent_->stdin_stream_ << data << std::flush;
                }
#endif
            }
        };

        class InputStream {
            ProcessWrapper* parent_;
            bool is_stderr_;
        public:
            InputStream(ProcessWrapper* p, bool stderr = false) : parent_(p), is_stderr_(stderr) {}
            std::string read_all() {
#ifdef USE_BOOST_PROCESS_V2
                parent_->run_io(); // Update buffers
                return is_stderr_ ? parent_->stderr_buffer_ : parent_->stdout_buffer_;
#elif defined(USE_BOOST_PROCESS_V1)
                std::string result;
                std::string line;
                auto* stream = is_stderr_ ? parent_->stderr_stream_.get() : parent_->stdout_stream_.get();
                if (stream) {
                    while (std::getline(*stream, line)) {
                        result += line + '\n';
                    }
                }
                return result;
#endif
            }
        };

        OutputStream stdin_stream() { return OutputStream(this); }
        InputStream stdout_stream() { return InputStream(this, false); }
        InputStream stderr_stream() { return InputStream(this, true); }
    };

    // Simple process execution - handle namespace differences
    inline int run_script(const std::string& shell, const std::string& command_line, std::string& std_err) {
#ifdef USE_BOOST_PROCESS_V2
        boost::asio::io_context ctx;
        boost::asio::readable_pipe stderr_pipe{ctx};
        
        boost::process::v2::process proc(ctx, shell, {"-c", command_line},
                                       boost::process::v2::process_stdio{nullptr, nullptr, stderr_pipe});
        
        boost::system::error_code ec;
        std::array<char, 1024> buffer;
        
        while (proc.running()) {
            auto bytes_read = stderr_pipe.read_some(boost::asio::buffer(buffer), ec);
            if (!ec && bytes_read > 0) {
                std_err.append(buffer.data(), bytes_read);
            }
            if (ec == boost::asio::error::eof) break;
        }
        
        proc.wait();
        return proc.exit_code();
        
#elif defined(USE_BOOST_PROCESS_V1)
        // Try V1 namespace first (newer Boost versions)
        try {
            boost::process::v1::ipstream istd_err;
            boost::process::v1::child child(shell, "-c", command_line, 
                                           boost::process::v1::std_err > istd_err);
            
            std::string line;
            while (child.running() && std::getline(istd_err, line)) {
                std_err += line + '\n';
            }
            
            child.wait();
            return child.exit_code();
        } catch (...) {
            // Fallback: try main namespace (older Boost versions)
            try {
                boost::process::ipstream istd_err;
                boost::process::child child(shell, "-c", command_line, 
                                           boost::process::std_err > istd_err);
                
                std::string line;
                while (child.running() && std::getline(istd_err, line)) {
                    std_err += line + '\n';
                }
                
                child.wait();
                return child.exit_code();
            } catch (...) {
                #pragma message("Both V1 namespaces failed")
                return -1;
            }
        }
#endif
    }

    // Simplified search_executable function
    inline std::string search_executable(const std::string& name) {
#ifdef USE_BOOST_PROCESS_V2
        auto path = boost::process::v2::environment::find_executable(name);
        return path.string();
#elif defined(USE_BOOST_PROCESS_V1)
        try {
            // Try V1 namespace first
            #if __has_include(<boost/process/v1/search_path.hpp>)
                auto path = boost::process::v1::search_path(name);
            #else
                auto path = boost::process::search_path(name);
            #endif
            return path.string();
        } catch (...) {
            // Fallback: try main namespace
            try {
                auto path = boost::process::search_path(name);
                return path.string();
            } catch (...) {
                return name; // Last resort fallback
            }
        }
#else
        return name; // Fallback: assume it's in PATH
#endif
    }

} // namespace process_compat
