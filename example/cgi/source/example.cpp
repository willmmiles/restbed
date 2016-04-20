/*
 * Example illustrating asio usage
 *
 * Server Usage:
 *    ./asio
 *
 * Client Usage:
 *    curl -w'\n' -v -X POST --data 'Hello, Restbed' 'http://localhost:1984/resource'
 */

#include <memory>
#include <deque>
#include <cstdlib>
#include <cstdio>
#include <restbed>
#include <spawn.h>

#include <asio/io_service.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/write.hpp>

#ifndef OPEN_MAX
#define OPEN_MAX 4096
#endif

using namespace std;
using namespace restbed;
using namespace asio;
using asio::posix::stream_descriptor;
using std::placeholders::_1;
using std::placeholders::_2;
    
constexpr int XFER_BUF_SIZE = 4096;

static const char* COMMAND = "sort";
static /*const*/ char* ARGS[] = { "sort", NULL };


/* Useful async utilities */

class simple_async_queue : public enable_shared_from_this<simple_async_queue>
{    
public:
    simple_async_queue(function<void(const Bytes&, function<void()>) > send) : m_outbuf(), m_send(send) {}

    void post(const Bytes& b) { 
        m_outbuf.push_back(b);
        if (m_outbuf.size() > 1)
            return;
        this->write();
    }
    
    void post(const Bytes&& b) { 
        m_outbuf.push_back(b);
        if (m_outbuf.size() > 1)
            return;
        this->write();
    }

    static void complete_write( shared_ptr<simple_async_queue> q ) {
        q->m_outbuf.pop_front();
        if (!q->m_outbuf.empty()) {
            q->write();
        }
    }

private:
      
    void write() {
        m_send(m_outbuf.front(), bind(complete_write,shared_from_this()));
    }

    deque<Bytes> m_outbuf;
    function<void(const Bytes&, function<void()>) > m_send;        
};


//! Define a FixedPoint operator.
template<typename Result, typename F>
struct FixedPoint {
  F m_function;

  template<typename Parameter1, typename Parameter2>
  Result operator()(Parameter1 parameter1, Parameter2 parameter2) const {
    return m_function(*this, parameter1, parameter2);
  }
  

  template<typename Parameter1, typename Parameter2, typename Parameter3>
  Result operator()(Parameter1 parameter1, Parameter2 parameter2, Parameter3 parameter3) const {
    return m_function(*this, parameter1, parameter2, parameter3);
  }
  
  FixedPoint(F function)
      : m_function(function) {}
};

//! Define a helper function to make it easy to create recursive functions.
template<typename Result, typename F>
FixedPoint<Result, F> MakeRecursive(F&& f) {
  return FixedPoint<Result, F>(std::forward<F>(f));
}

static bool is_open(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

static int spawn_child(int c_stdin, int c_stdout, int c_stderr) {
    posix_spawn_file_actions_t actions;    
    posix_spawn_file_actions_init(&actions);
    
    // Ensure all file descriptors are closed, except for the three specified as arguments
    for (int fd = 0; fd < OPEN_MAX; ++fd)
    {
        if ((fd == c_stdin) || (fd == c_stdout) || (fd == c_stderr)) continue;
        if (is_open(fd)) {
            posix_spawn_file_actions_addclose(&actions,fd);
        };
    };

    posix_spawn_file_actions_adddup2(&actions, c_stdin, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, c_stdout, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, c_stderr, STDERR_FILENO);
    
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
        
    // Execute the command
    int rv = posix_spawnp(NULL, COMMAND, &actions, NULL, ARGS, NULL);    
    
    // Close our end of the pipes
    close(c_stdin);
    close(c_stdout);
    close(c_stderr);
    
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    
    return rv;
}


static void post_method_handler( const shared_ptr< io_service >& io, const shared_ptr< Session >& session )
{
    const auto request = session->get_request( );
    
    int content_length = 0;
    request->get_header( "Content-Length", content_length );
    
    // Spawn handler process
    int h_stdin_fds[2], h_stdout_fds[2], h_stderr_fds[2];
    
    pipe(h_stdin_fds);
    pipe(h_stdout_fds);
    pipe(h_stderr_fds);
    
    // Wrap in ASIO objects
    auto h_stdin = make_shared<stream_descriptor>(*io, h_stdin_fds[1]);
    auto h_stdout = make_shared<stream_descriptor>(*io, h_stdout_fds[0]);
    auto h_stderr = make_shared<stream_descriptor>(*io, h_stderr_fds[0]);
    
    // Set up the child process
    if (spawn_child(h_stdin_fds[0], h_stdout_fds[1], h_stderr_fds[1]) == -1)
    {
        session->close( 500, "Unable to spawn process", { { "Content-Length", "22" } } );        
        return;
    }
    
    // Build the ASIO workflow
    // child -> restbed
    // First send OK, then follow up with data from the handle
    session->yield( OK, "", [=]( const shared_ptr< Session > session) {
        // Queue: writes to session
        auto stdout_queue = make_shared<simple_async_queue>([=](const Bytes& data, function<void()> f_continue)
        {
            session->yield( data, [=]( const shared_ptr< Session> session ) {
                if ( session->is_open() ) f_continue();
            });
        });    
    
        // pipe: read to queue
        auto buf = make_shared<array<char, XFER_BUF_SIZE> >();
        h_stdout->async_read_some( asio::buffer(*buf), MakeRecursive<void>(
            [=]( const std::function< void ( std::error_code ec, std::size_t length ) >& self, std::error_code ec, std::size_t length )
            {
                if ( (!ec) && session->is_open() ) {
                    stdout_queue->post( Bytes( buf->begin(), buf->begin() + length ) );
                    h_stdout->async_read_some( asio::buffer(*buf), bind( self, _1, _2 ) );
                }
            }
        ));        
    });               

    // restbed -> child
    // Queue: writes to pipe
    auto stdin_queue = make_shared<simple_async_queue>([=](const Bytes& data, function<void()> f_continue)
    {
        async_write( *h_stdin, asio::buffer( data.data(), data.size() ), [=]( std::error_code ec, std::size_t )
        {
            if (!ec)
            {                
                f_continue();
            }
            else
            {
                session->close( 500, "Write error", { { "Content-Length", "11" } } );        
            }                
        });
    });

    // Session: fetch to queue
    session->fetch( std::min(content_length, XFER_BUF_SIZE), bind( 
        MakeRecursive<void>(
            [=](const std::function<void(const shared_ptr< Session >&, const Bytes &, int)>& self, const shared_ptr< Session >& session, const Bytes & body, int remaining )
            {
                stdin_queue->post(body);
                remaining -= body.size();        
                if (remaining > 0)
                {
                    session->fetch( std::min( remaining, XFER_BUF_SIZE ), bind( self, _1, _2, remaining ) );
                }                 
            }
        ),
        _1, _2, content_length));


    // TODO: stderr handling
}


int main( const int, const char** )
{
    auto io = make_shared<io_service>();
    
    auto resource = make_shared< Resource >( );
    resource->set_path( "/resource" );
    resource->set_method_handler( "POST", [io](const shared_ptr< Session >& session) { post_method_handler(io, session); } );
    
    auto settings = make_shared< Settings >( );
    settings->set_port( 1984 );
    settings->set_default_header( "Connection", "close" );
    
    Service service( io );
    service.publish( resource );
    service.start( settings );
    
    return EXIT_SUCCESS;
}
