// #include <boost/asio/ip/tcp.hpp>
// #include <boost/beast/core.hpp>
// #include <boost/beast/http.hpp>
// #include <boost/beast/version.hpp>
// #include <boost/config.hpp>
// #include <cstdlib>
// #include <iostream>
// #include <memory>
// #include <string>
// #include <thread>
//
// using tcp = boost::asio::ip::tcp;
// namespace http = boost::beast::http;
//
// std::string defaultBody = "hey!";
//
// // This function produces an HTTP response for the given
// // request. The type of the response object depends on the
// // contents of the request, so the interface requires the
// // caller to pass a generic lambda for receiving the response.
// template <class Body, class Allocator, class Send>
// void handle_request(
//     http::request<Body, http::basic_fields<Allocator>> &&req, Send &&send) {
//
//   // Returns a not found response
//   auto const not_found = [&req](boost::beast::string_view target) {
//     http::response<http::string_body> res{http::status::not_found,
//                                           req.version()};
//     res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
//     res.set(http::field::content_type, "text/html");
//     res.keep_alive(req.keep_alive());
//     res.body() = "The resource '" + target.to_string() + "' was not found.";
//     res.prepare_payload();
//     return res;
//   };
//
//   return send(not_found(req.target()));
// }
//
// //------------------------------------------------------------------------------
//
// // Report a failure
// void fail(boost::system::error_code ec, char const *what) {
//   std::cerr << what << ": " << ec.message() << "\n";
// }
//
// // This is the C++11 equivalent of a generic lambda.
// // The function object is used to send an HTTP message.
// template <class Stream>
// struct send_lambda {
//   Stream &stream_;
//   bool &close_;
//   boost::system::error_code &ec_;
//
//   explicit send_lambda(
//       Stream &stream, bool &close, boost::system::error_code &ec)
//       : stream_(stream), close_(close), ec_(ec) {}
//
//   template <bool isRequest, class Body, class Fields>
//   void operator()(http::message<isRequest, Body, Fields> &&msg) const {
//     // Determine if we should close the connection after
//     close_ = msg.need_eof();
//
//     // We need the serializer here because the serializer requires
//     // a non-const file_body, and the message oriented version of
//     // http::write only works with const messages.
//     http::serializer<isRequest, Body, Fields> sr{msg};
//     http::write(stream_, sr, ec_);
//   }
// };
//
// // Handles an HTTP server connection
// void handle(tcp::socket &socket) {
//   bool close = false;
//   boost::system::error_code ec;
//
//   // This buffer is required to persist across reads
//   boost::beast::flat_buffer buffer;
//
//   // This lambda is used to send messages
//   send_lambda<tcp::socket> lambda{socket, close, ec};
//
//   for (;;) {
//     // Read a request
//     http::request<http::string_body> req;
//     http::read(socket, buffer, req, ec);
//     if (ec == http::error::end_of_stream)
//       break;
//     if (ec)
//       return fail(ec, "read");
//
//     // Send the response
//     handle_request(std::move(req), lambda);
//     if (ec)
//       return fail(ec, "write");
//     if (close) {
//       // This means we should close the connection, usually because
//       // the response indicated the "Connection: close" semantic.
//       break;
//     }
//   }
//
//   // Send a TCP shutdown
//   socket.shutdown(tcp::socket::shutdown_send, ec);
//
//   // At this point the connection is closed gracefully
// }

// int main(int argc, char *argv[]) {
//   if (argc != 3) {
//     std::cerr << "Usage: " << argv[0] << " <address> <port>" << std::endl;
//     return 1;
//   }
//
//   auto const address = boost::asio::ip::make_address(argv[1]);
//   auto const port = static_cast<unsigned short>(std::atoi(argv[2]));
//
//   boost::asio::io_context ioc(1);
//
//   tcp::acceptor acceptor(ioc, {address, port});
//   while (true) {
//     tcp::socket s{ioc};
//     acceptor.accept(s);
//     std::thread(std::bind(&handle, std::move(s))).detach();
//   }
// }
