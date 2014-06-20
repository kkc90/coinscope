#include <iostream>
#include <iterator>

#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "command_handler.hpp"
#include "bitcoin_handler.hpp"
#include "netwrap.hpp"

using namespace std;

namespace bc = bitcoin;

namespace ctrl {

handler_set g_active_handlers;

uint32_t handler::id_pool = 0;

void handler::handle_message_recv(const struct command_msg *msg) { 
	/* do control command handler crap */
	vector<uint8_t> out;

	bool writing = to_write;

	if (msg->command == COMMAND_GET_CXN) {
		/* format is 64 bit id, inaddr,  port */
		uint8_t buffer[sizeof(void*) + sizeof(in_addr)+sizeof(uint16_t)];
		for(auto it = bc::g_active_handlers.cbegin(); it != bc::g_active_handlers.cend(); ++it) {
			memcpy(buffer, &*it, sizeof(void*));
			struct in_addr addr = (*it)->get_remote_addr();
			uint16_t port = (*it)->get_remote_port();
			memcpy(buffer+sizeof(void*), &addr, sizeof(in_addr));
			memcpy(buffer+sizeof(void*)+sizeof(in_addr), &port, sizeof(uint16_t));
		}
		out.reserve(sizeof(buffer));
		copy(buffer, buffer + sizeof(buffer), back_inserter(out));
		iobuf_spec::append(&write_queue, out.data(), out.size());
		to_write += out.size();
	}

	if (!writing && to_write) {
		io.set(ev::WRITE);
	}


}

void handler::io_cb(ev::io &watcher, int revents) {
	if ((state & RECV_MASK) && (revents & ev::READ)) {
		ssize_t r(1);
		while(r > 0) { 
			do {
				r = read(watcher.fd, read_queue.offset_buffer(), to_read);
				if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN) { cerr << strerror(errno) << endl; }
				if (r > 0) {
					to_read -= r;
					read_queue.seek(read_queue.location() + r);
				}
			} while (r > 0 && to_read > 0);

			if (to_read == 0) {
				/* item needs to be handled */
				switch(state & RECV_MASK) {
				case RECV_HEADER:
					/* interpret data as message header and get length, reset remaining */ 
					{
						struct message *msg = (struct message*)read_queue.raw_buffer();
						to_read = msg->length;
						if (to_read == 0) { /* payload is packed message */
							if (msg->message_type == REGISTER) {
								/* msg->payload should be zero length here */
								/* send back their new user id */
							} else {
								/* command and bitcoin payload messages always have a payload */
								cerr << "bad message?" << endl;
							}
							read_queue.seek(0);
							to_read = sizeof(struct message);
						} else {
							read_queue.reserve(sizeof(message) + to_read);
							state = (state & SEND_MASK) | RECV_PAYLOAD;
						}
					}
					break;
				case RECV_PAYLOAD:
					{
						struct message *msg = (struct message*) read_queue.raw_buffer();
						if (msg->message_type == BITCOIN_PACKED_MESSAGE) {
							/* register message and send back its id */
						} else if (msg->message_type == CONNECT) {
							/* format is remote_inaddr, remote_port, local_inaddr, local_port (see command structures) */
							struct connect_payload *payload = (struct connect_payload*) msg->payload;
							int fd(-1);
							struct sockaddr_in addr;
							try {
								int sfd = Socket(AF_UNIX, SOCK_STREAM, 0);
								/* This socket is NOT non-blocking. Is this an issue in building up connections? */
								bzero(&addr,sizeof(addr));
								addr.sin_family = AF_INET;
								addr.sin_port = payload->remote_port;
								memcpy(&addr.sin_addr, &payload->remote_inaddr, sizeof(struct in_addr));
								fd = Connect(sfd, (struct sockaddr*)&addr, sizeof(addr));
								fcntl(fd, F_SETFD, O_NONBLOCK);
							} catch (network_error &e) {
								cerr << e.what() << endl;
							}
							if (fd >= 0) {
								bc::g_active_handlers.emplace(new bc::handler(fd, bc::SEND_VERSION_INIT, addr.sin_addr, addr.sin_port, payload->local_inaddr, payload->local_port));
							}

						} else if (msg->message_type == COMMAND) {
							handle_message_recv((struct command_msg*) msg->payload);
							read_queue.seek(0);
							to_read = sizeof(struct command_msg);
							state = (state & SEND_MASK) | RECV_HEADER;
						} else {
							cerr << "bad payload?" << endl;
						}
					}
					break;
				}

			}
		}
         
	}

	if (revents & ev::WRITE) {

		ssize_t r(1);
		while (to_write && r > 0) {
			r = write(watcher.fd, write_queue.offset_buffer(), to_write);
			if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN) { cerr << strerror(errno) << endl; }
			if (r > 0) {
				write_queue.seek(write_queue.location() + r);
			}
		}

		if (to_write == 0) {
			/* unregister write event! */
			state &= ~SEND_MASK;
			write_queue.seek(0);

		}
	}
}


accept_handler::accept_handler(int fd) { 
	io.set<accept_handler, &accept_handler::io_cb>(this);
	io.set(fd, ev::READ);
	io.start();
}

accept_handler::~accept_handler() {
	io.stop();
	close(io.fd);
}


void accept_handler::io_cb(ev::io &watcher, int revents) {
	struct sockaddr addr;
	socklen_t len;
	int client;
	try {
		client = Accept(watcher.fd, &addr, &len);
	} catch (network_error &e) {
		if (e.error_code() != EWOULDBLOCK && e.error_code() != EAGAIN) {
			cerr << e.what() << endl;
			watcher.stop();
			close(watcher.fd);

			/* not sure entirely what recovery policy should be on dead control channels, probably reattempt acquisition, this is a TODO */
			/*
			if (watchers.erase(watcher)) {
				delete this;
			}
			*/
		}
		return;
	}

	g_active_handlers.emplace(new handler(client));
}


};
