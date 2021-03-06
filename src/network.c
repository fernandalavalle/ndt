/**
 * This file contains the functions needed to handle network related
 * stuff.
 *
 * Jakub S�awi�ski 2006-05-30
 * jeremian@poczta.fm
 */

#include <assert.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string.h>
#include <unistd.h>
#include "jsonutils.h"

#include "logging.h"
#include "network.h"
#include "websocket.h"

/**
 * Create and bind socket.
 * @param addr I2Addr structure, where the new socket will be stored
 * @param serv the port number
 * @param family the ip family to try binding to
 * @param options the binding socket options
 * @returns The socket descriptor or error code (<0).
 *   Error codes:
 *     -1 : Unable to set socket address/port/file descriptor in address
 *          record "addr"
 *     -2 : Unable to set socket options
 */

static int OpenSocket(I2Addr addr, char* serv, int family, int options) {
  int fd = -1;
  int return_code = 0;

  struct addrinfo *fai = NULL;
  if (!(fai = I2AddrAddrInfo(addr, NULL, serv))) {
    return -2;
  }

  // Attempt to connect to one of the chosen addresses.
  struct addrinfo* ai = NULL;
  for (ai = fai; ai; ai = ai->ai_next) {
    if (ai->ai_family != family)
      continue;

    // create socket with obtained address domain, socket type and protocol
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

    // socket create failed. Abandon further activities using this socket
    if (fd < 0)
      continue;

    // allow sockets to reuse local address while binding unless there
    // is an active listener. If unable to set this option, indicate failure
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
      return_code = -2;
      goto failsock;
    }
    // end trying to set socket option to reuse local address

#ifdef AF_INET6
#ifdef IPV6_V6ONLY
    if (family == AF_INET6 && (options & OPT_IPV6_ONLY)) {
      on = 1;
      // the IPv6 version socket option setup
      if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) != 0) {
        return_code = -2;
        goto failsock;
      }
    }
#endif
#endif

    // try to bind to address
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {  // successful
      // set values in "addr" structure
      if (!I2AddrSetSAddr(addr, ai->ai_addr, ai->ai_addrlen) ||
          !I2AddrSetProtocol(addr, ai->ai_protocol) ||
          !I2AddrSetSocktype(addr, ai->ai_socktype)) {
        log_println(1, "OpenSocket: Unable to set saddr in address record");
        return_code = -1;
        goto failsock;
      }
      // set port if not already done, else return -1
      if (!I2AddrPort(addr)) {
        struct sockaddr_storage tmp_addr;
        socklen_t tmp_addr_len = sizeof(tmp_addr);
        I2Addr tmpAddr;
        if (getsockname(fd, (struct sockaddr*) &tmp_addr,
                        &tmp_addr_len)) {
          log_println(1, "OpenSocket: Unable to getsockname in address record");
          return_code = -1;
          goto failsock;
        }
        tmpAddr = I2AddrBySAddr(
            get_errhandle(), (struct sockaddr*) &tmp_addr, tmp_addr_len, 0, 0);
        I2AddrSetPort(addr, I2AddrPort(tmpAddr));
        I2AddrFree(tmpAddr);
      }
      // save socket file descriptor
      if (!I2AddrSetFD(addr, fd, True)) {
        log_println(1, "OpenSocket: Unable to set file descriptor in address "
                    "record");
        return_code = -1;
        goto failsock;
      }
      // end setting values in "addr" structure

      break;
    }

    // Address is indicated as being in use. Display actual socket options to
    // user and return
    if (errno == EADDRINUSE) {
      /* RAC debug statemement 10/11/06 */
      socklen_t onSize = sizeof(on);
      getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, &onSize);
      log_println(1, "bind(%d) failed: Address already in use given as the "
                  "reason, getsockopt() returned %d", fd, on);
    } else {
      log_println(1, "bind(%d) failed: %s", fd, strerror(errno));
    }
    return_code = -1;
    goto failsock;
  }

  return fd;

  // If opening socket failed, print error, and try to close socket
  // file-descriptor.
 failsock:
  while ((close(fd) < 0) && (errno == EINTR)) { }
  return return_code;
}

/**
 * Createsthe I2Addr structure with the listen socket.
 * @param addr  the I2Addr structure, where listen socket should
 *                   be added, or NULL, if the new structure should be
 *                   created
 * @param serv  the port number
 * @param options the binding socket options
 * @param buf_size manually set the TCP send/receive socket buffer
 * @returns I2Addr structure with the listen socket.
 */

I2Addr CreateListenSocket(I2Addr addr, char* serv, int options, int buf_size) {
  int fd = -1;
  socklen_t optlen;
  int set_size;

  if (addr && (I2AddrFD(addr) > -1)) {
    log_println(1, "Invalid I2Addr record - fd already specified.");
    goto error;
  }

  if ((!addr) &&
      !(addr = I2AddrByWildcard(get_errhandle(), SOCK_STREAM, serv))) {
    log_println(1, "Unable to create I2Addr record by wildcard.");
    goto error;
  }

  if (!I2AddrSetPassive(addr, True)) {
    log_println(1, "Unable to set passive mode in I2Addr record.");
    goto error;
  }

  // create and bind socket using arguments, prefering v6 (since v6 addresses
  // can be both v4 and v6).
#ifdef AF_INET6
  if ((options & OPT_IPV4_ONLY) == 0)
    fd = OpenSocket(addr, serv, AF_INET6, options);
#endif
  if (fd < 0)
    if ((options & OPT_IPV6_ONLY) == 0)
      fd = OpenSocket(addr, serv, AF_INET, options);

  if (fd < 0) {
    log_println(1, "Unable to open socket.");
    goto error;
  }

  /* Set sock opt code from Marion Nakanson <hakansom@ohsu.edu
   *  OHSU Advanced Computing Center
   * email on 2/19/09 correctly notes that setsockops must be made before open()
   * or listen() calls are made
   */

  optlen = sizeof(set_size);
  // get send buffer size
  getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &set_size, &optlen);
  log_println(5, "Send buffer initialized to %d, ", set_size);

  // get receive buffer size
  getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &set_size, &optlen);
  log_println(5, "Receive buffer initialized to %d", set_size);

  // now assign buffer sizes passed as arguments
  if (buf_size > 0) {
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    // print values set to help user verify
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &set_size, &optlen);
    log_print(5, "Changed buffer sizes: Send buffer set to %d(%d), ",
              set_size, buf_size);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &set_size, &optlen);
    log_println(5, "Receive buffer set to %d(%d)", set_size, buf_size);
  }

  // listen on socket for connections, with backlog queue length = NDT_BACKLOG
  if (listen(fd, NDT_BACKLOG) < 0) {  // if listen returns value <0, then error
    log_println(1, "listen(%d,%d):%s", fd, NDT_BACKLOG, strerror(errno));
    goto error;
  }

  return addr;

  // If error, try freeing memory
 error:
  I2AddrFree(addr);
  return NULL;
}

/**
 * Create the connect socket and adds it to the I2Addr
 *              structure.
 * @param sockfd  target place for the socket descriptor
 * @param local_addr  I2Addr structure with the local address
 *                         to bind the connect socket to
 * @param server_addr  I2Addr structure with the remote
 *                          server address
 * @param options connect socket options
 * @param buf_size manually set the TCP send/receive buffer size`
 * @return   0 - success,
 *          !0 - error code.
 */

int CreateConnectSocket(int* sockfd, I2Addr local_addr, I2Addr server_addr,
                        int options, int buf_size) {
  struct addrinfo *fai = NULL;
  struct addrinfo *ai = NULL;
  struct addrinfo *lfai = NULL;
  struct addrinfo *lai = NULL;
  socklen_t optlen;
  int set_size;

  assert(sockfd);
  assert(server_addr);

  if (!server_addr) {
    log_println(1, "Invalid server address");
    goto error;
  }

  // already connected and bound
  if ((*sockfd = I2AddrFD(server_addr)) > -1) {
    return 0;
  }

  if (!(fai = I2AddrAddrInfo(server_addr, NULL, NULL))) {
    log_println(1, "Failed to get address info for server address");
    goto error;
  }

  int family = AF_UNSPEC;
#ifdef AF_INET6
  // options provided by user indicate V6 or V4 only
  if ((options & OPT_IPV6_ONLY) != 0)
    family = AF_INET6;
#endif
  else if ((options & OPT_IPV4_ONLY) != 0)
    family = AF_INET;

  for (ai = fai; ai; ai = ai->ai_next) {
    if (family != AF_UNSPEC && ai->ai_family != family) {
      log_println(1, "Skipping family %d", family);
      continue;
    }

    // create socket with obtained address domain, socket type and protocol
    *sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (*sockfd < 0) {
      // socket create failed. Abandon further activities using this socket
      log_println(1, "Failed to create %d %d %d", ai->ai_family,
                  ai->ai_socktype, ai->ai_protocol);
      continue;
    }

    // local address has been specified. Get details and bind to this adderess
    if (local_addr) {
      printf("local_addr\n");
      int bindFailed = 1;
      if (!(lfai = I2AddrAddrInfo(local_addr, NULL, NULL)))
        continue;

      // Validate INET address family
      for (lai = lfai; lai; lai = lai->ai_next) {
        if (lai->ai_family != family)
          continue;

        // bind to local address
        if (bind((*sockfd), lai->ai_addr, lai->ai_addrlen) == 0) {
          bindFailed = 0;  // bind successful
          break; /* success */
        }
      }

      // Failed to bind. Close socket file-descriptor and move on
      if (bindFailed == 1) {
        close((*sockfd)); /* ignore this one */
        continue;
      }
    }  // end local address

    /* Set sock opt code from Marion Nakanson <hakansom@ohsu.edu
     *  OHSU Advanced Computing Center
     * email on 2/19/09 correctly notes that setsockops must be made before open()
     * or listen() calls are made
     */

    optlen = sizeof(set_size);
    // get send buffer size for logs
    getsockopt(*sockfd, SOL_SOCKET, SO_SNDBUF, &set_size, &optlen);
    log_print(5, "\nSend buffer initialized to %d, ", set_size);
    // get receive buffer size for logs
    getsockopt(*sockfd, SOL_SOCKET, SO_RCVBUF, &set_size, &optlen);
    log_println(5, "Receive buffer initialized to %d", set_size);

    // now assign buffer sizes passed as arguments
    if (buf_size > 0) {
      setsockopt(*sockfd, SOL_SOCKET, SO_SNDBUF, &buf_size,
                 sizeof(buf_size));
      setsockopt(*sockfd, SOL_SOCKET, SO_RCVBUF, &buf_size,
                 sizeof(buf_size));
      // log values for reference
      getsockopt(*sockfd, SOL_SOCKET, SO_SNDBUF, &set_size, &optlen);
      log_print(5, "Changed buffer sizes: Send buffer set to %d(%d), ",
                set_size, buf_size);
      getsockopt(*sockfd, SOL_SOCKET, SO_RCVBUF, &set_size, &optlen);
      log_println(5, "Receive buffer set to %d(%d)", set_size, buf_size);
    }

    // Connect to target socket
    if (connect(*sockfd, ai->ai_addr, ai->ai_addrlen) == 0) {
      log_println(1, "Connected!");
      // save server address values
      if (I2AddrSetSAddr(server_addr, ai->ai_addr, ai->ai_addrlen) &&
          I2AddrSetSocktype(server_addr, ai->ai_socktype) &&
          I2AddrSetProtocol(server_addr, ai->ai_protocol) &&
          I2AddrSetFD(server_addr, *sockfd, True)) {
        log_println(1, "Client socket created");
        return 0;
      }
      // unable to save
      log_println(1, "I2Addr functions failed after successful connection");
      while ((close(*sockfd) < 0) && (errno == EINTR)) { }
      return 1;
    } else {
      log_println(0, "Failed to connect: %s", strerror(errno));
      // goto error;
    }
  }

  log_println(0, "No sockets could be created that match requirements.");

 error:
  return -1;
}

/**
 * Converts message to JSON format and sends it to the control Connection.
 * @param ctl control Connection
 * @param type type of the message
 * @param msg message to send
 * @param len length of the message
 * @param connectionFlags indicates if JSON format is supported by the other side (connectionFlags & JSON_SUPPORT)
 *                        and if websockets are supported by the other side (connectionFlags & WEBSOCKET_SUPPORT)
 *                        It is expected, but not required, that WEBSOCKET_SUPPORT will always include JSON_SUPPORT.
 * @param jsonConvertType defines how message converting should be handled:
 *			JSON_SINGLE_VALUE: single key/value pair is being created (using default key)
 *							   with msg as value
 *			JSON_MULTIPLE_VALUES: multiple key/values pairs are being created using
 *                                keys, keys_delimiters, values and values_delimiters
 *                                params
 *			JSON_KEY_VALUE_PAIRS: given message contains one or many key/value pairs
 *                                with following syntax: key1: value1
 *                                                       key2: value2 etc
 * @param keys buffer containing keys' values (used only when jsonConvertType is set to
 * 			   JSON_MULTIPLE_VALUES)
 * @param keysDelimiters delimiters for keys parameter (used only when jsonConvertType is set to
 * 			   JSON_MULTIPLE_VALUES)
 * @param values buffer containing map values, key-value matching is being done by order in which
 * 		  		 they appear in keys and values params (used only when jsonConvertType is set to
 * 			     JSON_MULTIPLE_VALUES)
 * @param valuesDelimiters delimiters for values parameter (used only when jsonConvertType is
 * 			   set to JSON_MULTIPLE_VALUES)
 *
 * @return 0 on success, error code otherwise
 *        Error codes:
 *        -1 - Cannot write to socket at all
 *        -2 - Cannot complete writing full message data into socket
 *        -3 - Cannot write after retries
 *        -4 - Cannot convert msg to JSON
 *
 */
int send_json_msg_any(Connection* ctl, int type, const char* msg, int len,
                      int connectionFlags, int jsonConvertType,
                      const char *keys, const char *keysDelimiters,
                      const char *values, char *valuesDelimiters) {
  char* tempBuff;
  int ret = 0;
  // if JSON is not supported by second side, sends msg as it is
  if (!(connectionFlags & JSON_SUPPORT)) {
    if (connectionFlags & WEBSOCKET_SUPPORT) {
      return send_websocket_msg(ctl, type, msg, len);
    } else {
      return send_msg_any(ctl, type, msg, len);
    }
  }

  switch(jsonConvertType) {
    case JSON_SINGLE_VALUE:
      tempBuff = json_create_from_single_value(msg); break;
    case JSON_MULTIPLE_VALUES:
      tempBuff = json_create_from_multiple_values(keys, keysDelimiters,
                                                  values, valuesDelimiters); break;
    case JSON_KEY_VALUE_PAIRS:
      tempBuff = json_create_from_key_value_pairs(msg); break;
    default:
      if (connectionFlags & WEBSOCKET_SUPPORT) {
        return send_websocket_msg(ctl, type, msg, len);
      } else {
        return send_msg_any(ctl, type, msg, len);
      }
  }

  if (!tempBuff) {
    return -4;
  }
  if (connectionFlags & WEBSOCKET_SUPPORT) {
    ret = send_websocket_msg(ctl, type, tempBuff, strlen(tempBuff));
  } else {
    ret = send_msg_any(ctl, type, tempBuff, strlen(tempBuff));
  }
  free(tempBuff);
  return ret;
}

/**
 * Shortest version of send_json_msg method. Uses default NULL values for
 * JSON_MULTIPLE_VALUES convert type specific parameters.
 */
int send_json_message_any(Connection* ctl, int type, const char* msg, int len,
                      int connectionFlags, int jsonConvertType) {
  return send_json_msg_any(ctl, type, msg, len, connectionFlags, jsonConvertType,
                           NULL, NULL, NULL, NULL);
}

/**
 * Sends the protocol message to the control connection.
 * @param ctl control Connection
 * @param type type of the message
 * @param msg message to send
 * @param len length of the message
 * @return 0 on success, -1 otherwise
 */
int send_msg_any(Connection* ctl, int type, const void* msg, int len) {
  unsigned char buff[3];

  assert(msg);
  assert(len >= 0);

  /*  memset(0, buff, 3); */
  // set message type and length into message itself
  buff[0] = type;
  buff[1] = len >> 8;
  buff[2] = len;

  // Write initial data about length and type to socket
  if (writen_any(ctl, buff, 3) != 3) return -1;

  // Now write the actual message
  if (writen_any(ctl, msg, len) != len) return -1;
  log_println(8, ">>> send_msg: type=%d, len=%d, msg=%s, pid=%d", type, len,
              msg, getpid());

  protolog_sendprintln(type, msg, len, getpid(), ctl->socket);

  return 0;
}

/**
 * Receive the protocol message from the control socket.
 * @param ctl control Connection
 * @param type target place for type of the message
 * @param msg target place for the message body
 * @param len target place for the length of the message
 * @returns 0 on success, error code otherwise.
 *          Error codes:
 *          -1 : Error reading from socket
 *          -2 : No of bytes received were lesser than expected byte count
 *          -3 : No of bytes received did not match expected byte count
 */
int recv_msg_any(Connection* ctl, int* type, void* msg, int* len) {
  unsigned char buff[3];
  int length;
  char *msgtemp = (char*) msg;

  assert(type);
  assert(msg);
  assert(len);

  // if 3 bytes are not explicitly read, signal error
  if (readn_any(ctl, buff, 3) != 3) {
    return -1;
  }

  // get msg type, and calculate length as sum of the next 2 bytes
  *type = buff[0];
  length = buff[1];
  length = (length << 8) + buff[2];

  // if received buffer size < length as conveyed by buffer contents, then error
  assert(length <= (*len));
  if (length > (*len)) {
    log_println(3, "recv_msg: length [%d] > *len [%d]", length, *len);
    return -2;
  }
  *len = length;
  if (readn_any(ctl, msg, length) != length) {
    return -3;
  }
  log_println(8, "<<< recv_msg: type=%d, len=%d", *type, *len);

  protolog_rcvprintln(*type, msgtemp, *len, getpid(), ctl->socket);

  return 0;
}

int recv_any_msg(Connection* conn, int* type, void* msg, int* len,
                 int connectionFlags) {
  if (connectionFlags & WEBSOCKET_SUPPORT) {
    return recv_websocket_ndt_msg(conn, type, msg, len);
  } else {
    return recv_msg_any(conn, type, msg, len);
  }
}

/**
 * Translates a numerical SSL error into a string.
 * @param ssl_err the SSL error
 * @return a char* with a string explaining the error code
 */
const char* ssl_error_str(int ssl_err) {
  switch (ssl_err) {
    case SSL_ERROR_NONE:
      return "No error occurred. Why was this called?";
    case SSL_ERROR_ZERO_RETURN:
      return "SSL_ERROR_ZERO_RETURN";
    case SSL_ERROR_WANT_READ:
      return "SSL_ERROR_WANT_READ (socket was not readable)";
    case SSL_ERROR_WANT_WRITE:
      return "SSL_ERROR_WANT_WRITE (socket was not writeable)";
    case SSL_ERROR_WANT_CONNECT:
      return "SSL_ERROR_WANT_CONNECT";
    case SSL_ERROR_WANT_ACCEPT:
      return "SSL_ERROR_WANT_ACCEPT";
    case SSL_ERROR_WANT_X509_LOOKUP:
      return "SSL_ERROR_WANT_X509_LOOKUP";
#ifdef SSL_ERROR_WANT_ASYNC
    case SSL_ERROR_WANT_ASYNC:
      return "SSL_ERROR_WANT_ASYNC";
#endif
#ifdef SSL_ERROR_WANT_ASYNC_JOB
    case SSL_ERROR_WANT_ASYNC_JOB:
      return "SSL_ERROR_WANT_ASYNC_JOB";
#endif
    case SSL_ERROR_SYSCALL:
      return "SSL_ERROR_SYSCALL";
    case SSL_ERROR_SSL:
      return "SSL_ERROR_SSL";
    default:
      return "UKNOWN_SSL_ERROR (this should never happen)";
  }
}

/**
 * Returns whether an SSL error is recoverable.
 * @param ssl_error the value from SSL_get_error
 * @param ssl_errno the value of errno after the offending SSL read or write
 * @return true if retrying might work, false otherwise
 */
int is_recoverable_ssl_error(int ssl_error, int ssl_errno) {
  switch (ssl_error) {
    case SSL_ERROR_WANT_WRITE:
      return 1;
    case SSL_ERROR_WANT_READ:
      return 1;
    case SSL_ERROR_SYSCALL:
      return (ssl_errno == EINTR) || (ssl_errno == EAGAIN);
    default:
      return 0;
  }
}

/**
 * Try a single write to a socket.
 * @param socketfd The socket
 * @param buf The data
 * @param amount The data size
 * @return The number of bytes written, -1 on fatal error, and 0 on recoverable
 *         error.
 */
int write_raw(int socketfd, const char* buf, int amount) {
  int n;
  n = write(socketfd, buf, amount);
  if (n == -1) {
    if (errno == EINTR || errno == EAGAIN) {
      // Recoverable errors
      return 0;
    } else {
      // Everything else is unrecoverable
      log_println(6,
                  "write_raw() Error! write(%d) failed with err=%s (%d) pid=%d",
                  socketfd, strerror(errno), errno, getpid());
      return -1;
    }
  } else {
    // Success!
    return n;
  }
}

/**
 * Try a single SSL_write to a socket.
 * @param ssl The ssl connection
 * @param buf The data
 * @param amount The data size
 * @return The number of bytes written, -1 on fatal error, and 0 on recoverable
 *         error.
 */
int write_ssl(SSL* ssl, const char* buf, int amount) {
  int n, ssl_error, ssl_errno;
  ERR_clear_error();
  n = SSL_write(ssl, buf, amount);
  ssl_errno = errno;
  if (n == 0) {
    // 0 represents fatal errors for SSL_write
    log_println(6, "write_ssl() Error! SSL_write() failed unrecoverably pid=%d",
                getpid());
    ssl_error = SSL_get_error(ssl, n);
    log_println(6, "SSL error: %s (%d, errno=%d)", ssl_error_str(ssl_error),
                ssl_error, ssl_errno);
    return -1;
  } else if (n < 0) {
    // Possibly a recoverable error
    ssl_error = SSL_get_error(ssl, n);
    // The only recoverable errors
    if (is_recoverable_ssl_error(ssl_error, ssl_errno)) {
      log_println(6, "SSL_write had a recoverable ssl error %s (%d, errno=%d)",
                  ssl_error_str(ssl_error), ssl_error, ssl_errno);
      return 0;
    } else {
      log_println(6, "write_ssl() Error! SSL_write() failed with err=%d pid=%d",
                  ssl_error, getpid());
      log_println(6, "SSL error: %s (%d, errno=%d)", ssl_error_str(ssl_error),
                  ssl_error, ssl_errno);
      return -1;
    }
  } else {
    // Success!
    return n;
  }
}

/**
 * Write the given amount of data to the Connection. When this function writes,
 * it will not return until all data is written, and it is insensitive to
 * EINTR. This means that when it writes to a bad pipe and gets errno set to
 * EINTR and then the process receives the SIGPIPE signal, it is incumbent upon
 * the SIGPIPE handler to exit the process.
 *
 * In the context of web100srv, this means that the server main loop should
 * never call writen, only the child should call writen, and that the signal
 * handler for SIGPIPE needs to exit() when a child receives SIGPIPE.
 *
 * @param conn the Connection
 * @param buf buffer with data to write
 * @param amount the size of the data
 * @return The amount of bytes written to the Connection.
 *         -1 when it gets an unrecoverable error, just like write().
 */
int writen_any(Connection* conn, const void* buf, int amount) {
  int sent, n;
  const char* ptr = buf;
  sent = 0;
  assert(amount >= 0);
  while (sent < amount) {
    if (conn->ssl == NULL) {
      n = write_raw(conn->socket, ptr + sent, amount - sent);
    } else {
      n = write_ssl(conn->ssl, ptr + sent, amount - sent);
    }
    if (n == -1) return -1;
    // success writing "n" bytes. Increment total bytes written
    sent += n;
  }
  return sent;
}

size_t readn_ssl(SSL *ssl, void *buf, size_t amount) {
  int received = 0;
  int ssl_err, ssl_errno;

  ERR_clear_error();
  received = SSL_read(ssl, buf, amount);
  ssl_errno = errno;
  if (received <= 0) {
    ssl_err = SSL_get_error(ssl, received);
    if (is_recoverable_ssl_error(ssl_err, ssl_errno)) {
      log_println(6, "SSL_read had a recoverable ssl error %s (%d, errno=%d)",
                  ssl_error_str(ssl_err), ssl_err, ssl_errno);
      return 0;
    } else {
      log_println(2, "SSL_read failed due to %s (%d, errno=%d)\n",
                  ssl_error_str(ssl_err), ssl_err, ssl_errno);
      return -1;
    }
  }
  return received;
}

/**
 * Reads from a raw socket.
 *
 * @param fd The file descriptor to read from
 * @param buf The buffer to read into
 * @param amount The the amount of data that can be read into buf
 * @return The number of bytes read, or a negative error code.
 */
int readn_raw(int fd, void *buf, size_t amount) {
  ssize_t received = 0;
  int error;  // A local variable to hold the contents of errno
  received = read(fd, buf, amount);
  if (received <= -1) {
    if ((error = errno) == EINTR) {
      return 0;
    } else {
      // An unrecoverable error occurred
      log_println(3, "readn_raw failed: %s (%d)", strerror(error), error);
      return -error;  // genuine socket error, return
    }
  } else if (received == 0) {
    // read() returning 0 means the fd is at EOF, which is a fatal error here.
    return -1;
  } else {
    return received;
  }
}

/**
 * Read the given amount of data from the Connection.
 * @param conn The connection to read
 * @param buf buffer for data
 * @param amount size of the data to read
 * @return The amount of bytes read from the Connection
 */
size_t readn_any(Connection *conn, void *buf, size_t amount) {
  assert(amount >= 0);
  size_t total_read = 0;
  char *ptr = buf;
  int received;

  while (total_read < amount) {
    if (conn->ssl != NULL) {
      received = readn_ssl(conn->ssl, ptr + total_read, amount - total_read);
    } else {
      received = readn_raw(conn->socket, ptr + total_read, amount - total_read);
    }
    if (received < 0) return 0;
    total_read += received;
  }
  return total_read;
}

/**
 * Shutdown the connection and free any resources associated with it.  After
 * this, neither this process nor any other process may use the Connection or
 * its underlying socket and SSL.
 * @param conn The connection to shutdown
 */
void shutdown_connection(Connection *conn) {
  if (conn->ssl != NULL) {
    SSL_shutdown(conn->ssl);
  }
  SSL_free(conn->ssl);
  conn->ssl = NULL;
  shutdown(conn->socket, SHUT_RDWR);
}

/**
 * Close the connection and free any resources associated with it.  Will not
 * shut the connection down - it may still be used by a fork()ed process.
 * @param conn The connection to close
 */
void close_connection(Connection *conn) {
  SSL_free(conn->ssl);
  conn->ssl = NULL;
  close(conn->socket);
}

/**
 * Open and set up an SSL connection from a socket connection.
 * @param conn the Connection to set up.  Should already have its socket set to
 *             a valid socketfd
 * @param ctx the SSL context to use for the setup
 * @return 0 or an error code
 */
int setup_SSL_connection(Connection *conn, SSL_CTX *ctx) {
  int ssl_err;
  int ssl_ret;
  int ssl_errno;
  ERR_clear_error();
  conn->ssl = SSL_new(ctx);
  if (conn->ssl == NULL) {
    log_println(4, "SSL_new failed");
    return ENOMEM;
  }
  ERR_clear_error();
  if (SSL_set_fd(conn->ssl, conn->socket) == 0) {
    log_println(4, "SSL_set_fd failed");
    return EIO;
  }
  do {
    ssl_err = 0;
    ERR_clear_error();
    ssl_ret = SSL_accept(conn->ssl);
    ssl_errno = errno;
    if (ssl_ret != 1) {
      ssl_err = SSL_get_error(conn->ssl, ssl_ret);
      if (!is_recoverable_ssl_error(ssl_err, ssl_errno)) {
        log_println(4, "SSL_accept failed: %s (%d, errno=%d)",
                    ssl_error_str(ssl_err), ssl_err, ssl_errno);
        return EIO;
      }
    }
  } while (ssl_ret != 1);
  return 0;
}
