#include "gsh.h"
#include <sys/uio.h>

static void setProtocolError(redisClient *c, int pos);

/* To evaluate the output buffer size of a client we need to get size of
 * allocated objects, however we can't used zmalloc_size() directly on sds
 * strings because of the trick they use to work (the header is before the
 * returned pointer), so we use this helper function. */
size_t zmalloc_size_sds(sds s) {
		return zmalloc_size(s-sizeof(struct sdshdr));
}

void *dupClientReplyValue(void *o) {
		incrRefCount((robj*)o);
		return o;
}


redisClient *createClient(int fd) {
		redisClient *c = zmalloc(sizeof(redisClient));
		c->bufpos = 0;

		anetNonBlock(NULL,fd);
		anetTcpNoDelay(NULL,fd);
		if (aeCreateFileEvent(server.el,fd,AE_READABLE,readQueryFromClient, c) == AE_ERR)
		{
				close(fd);
				zfree(c);
				return NULL;
		}

		selectDb(c,0);
		c->fd = fd;
		c->querybuf = sdsempty();
		c->reqtype = 0;
		c->argc = 0;
		c->argv = NULL;
		c->cmd = c->lastcmd = NULL;
		c->multibulklen = 0;
		c->bulklen = -1;
		c->sentlen = 0;
		c->flags = 0;
		c->lastinteraction = time(NULL);
		c->reply = listCreate();
		c->reply_bytes = 0;
		listSetFreeMethod(c->reply,decrRefCount);
		listSetDupMethod(c->reply,dupClientReplyValue);
		listAddNodeTail(server.clients,c);
		return c;
}

/* Set the event loop to listen for write events on the client's socket.
 * Typically gets called every time a reply is built. */
int _installWriteEvent(redisClient *c) {
		if (c->fd <= 0) return REDIS_ERR;
		if (c->bufpos == 0 && listLength(c->reply) == 0 &&
						aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR) return REDIS_ERR;
		return REDIS_OK;
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
robj *dupLastObjectIfNeeded(list *reply) {
		robj *new, *cur;
		listNode *ln;
		redisAssert(listLength(reply) > 0);
		ln = listLast(reply);
		cur = listNodeValue(ln);
		if (cur->refcount > 1) {
				new = dupStringObject(cur);
				decrRefCount(cur);
				listNodeValue(ln) = new;
		}
		return listNodeValue(ln);
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

int _addReplyToBuffer(redisClient *c, char *s, size_t len) {
		size_t available = sizeof(c->buf)-c->bufpos;

		if (c->flags & REDIS_CLOSE_AFTER_REPLY) return REDIS_OK;

		/* If there already are entries in the reply list, we cannot
		 * add anything more to the static buffer. */
		if (listLength(c->reply) > 0) return REDIS_ERR;

		/* Check that the buffer has enough space available for this string. */
		if (len > available) return REDIS_ERR;

		memcpy(c->buf+c->bufpos,s,len);
		c->bufpos+=len;
		return REDIS_OK;
}

void _addReplyObjectToList(redisClient *c, robj *o) {
		robj *tail;

		if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

		if (listLength(c->reply) == 0) {
				incrRefCount(o);
				listAddNodeTail(c->reply,o);
				c->reply_bytes += zmalloc_size_sds(o->ptr);
		} else {
				tail = listNodeValue(listLast(c->reply));

				/* Append to this object when possible. */
				if (tail->ptr != NULL &&
								sdslen(tail->ptr)+sdslen(o->ptr) <= REDIS_REPLY_CHUNK_BYTES)
				{
						c->reply_bytes -= zmalloc_size_sds(tail->ptr);
						tail = dupLastObjectIfNeeded(c->reply);
						tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));
						c->reply_bytes += zmalloc_size_sds(tail->ptr);
				} else {
						incrRefCount(o);
						listAddNodeTail(c->reply,o);
						c->reply_bytes += zmalloc_size_sds(o->ptr);
				}
		}
}


void _addReplyStringToList(redisClient *c, char *s, size_t len) {
		robj *tail;

		if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

		if (listLength(c->reply) == 0) {
				robj *o = createStringObject(s,len);

				listAddNodeTail(c->reply,o);
				c->reply_bytes += zmalloc_size_sds(o->ptr);
		} else {
				tail = listNodeValue(listLast(c->reply));

				/* Append to this object when possible. */
				if (tail->ptr != NULL &&
								sdslen(tail->ptr)+len <= REDIS_REPLY_CHUNK_BYTES)
				{
						c->reply_bytes -= zmalloc_size_sds(tail->ptr);
						tail = dupLastObjectIfNeeded(c->reply);
						tail->ptr = sdscatlen(tail->ptr,s,len);
						c->reply_bytes += zmalloc_size_sds(tail->ptr);
				} else {
						robj *o = createStringObject(s,len);

						listAddNodeTail(c->reply,o);
						c->reply_bytes += zmalloc_size_sds(o->ptr);
				}
		}
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */

void addReply(redisClient *c, robj *obj) {
		if (_installWriteEvent(c) != REDIS_OK) return;

		/* This is an important place where we can avoid copy-on-write
		 * when there is a saving child running, avoiding touching the
		 * refcount field of the object if it's not needed.
		 *
		 * If the encoding is RAW and there is room in the static buffer
		 * we'll be able to send the object to the client without
		 * messing with its page. */
		if (obj->encoding == REDIS_ENCODING_RAW) {
				if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
						_addReplyObjectToList(c,obj);
		} else {
				/* FIXME: convert the long into string and use _addReplyToBuffer()
				 * instead of calling getDecodedObject. As this place in the
				 * code is too performance critical. */
				obj = getDecodedObject(obj);
				if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
						_addReplyObjectToList(c,obj);
				decrRefCount(obj);
		}
}

void addReplyString(redisClient *c, char *s, size_t len) {
		if (_installWriteEvent(c) != REDIS_OK) return;
		if (_addReplyToBuffer(c,s,len) != REDIS_OK)
				_addReplyStringToList(c,s,len);
}


void _addReplyLongLong(redisClient *c, long long ll, char prefix) {
		char buf[128];
		int len;
		buf[0] = prefix;
		len = ll2string(buf+1,sizeof(buf)-1,ll);
		buf[len+1] = '\r';
		buf[len+2] = '\n';
		addReplyString(c,buf,len+3);
}

/* Add a C buffer as bulk reply */
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len) {
		_addReplyLongLong(c,len,'$');
		addReplyString(c,p,len);
		addReply(c,shared.crlf);
}


/* Add a C nul term string as bulk reply */
void addReplyBulkCString(redisClient *c, char *s) {
		if (s == NULL) {
				addReply(c,shared.nullbulk);
		} else {
				addReplyBulkCBuffer(c,s,strlen(s));
		}
}


void _addReplyError(redisClient *c, char *s, size_t len) {
		addReplyString(c,"-ERR ",5);
		addReplyString(c,s,len);
		addReplyString(c,"\r\n",2);
}

void addReplyError(redisClient *c, char *err) {
		_addReplyError(c,err,strlen(err));
}

void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
		va_list ap;
		va_start(ap,fmt);
		sds s = sdscatvprintf(sdsempty(),fmt,ap);
		va_end(ap);
		_addReplyError(c,s,sdslen(s));
		sdsfree(s);
}

void _addReplyStatus(redisClient *c, char *s, size_t len) {
		addReplyString(c,"+",1);
		addReplyString(c,s,len);
		addReplyString(c,"\r\n",2);
}

void addReplyStatusFormat(redisClient *c, const char *fmt, ...) {
		va_list ap;
		va_start(ap,fmt);
		sds s = sdscatvprintf(sdsempty(),fmt,ap);
		va_end(ap);
		_addReplyStatus(c,s,sdslen(s));
		sdsfree(s);
}


static void acceptCommonHandler(int fd) {
		redisClient *c;
		if ((c = createClient(fd)) == NULL) {
				redisLog(REDIS_WARNING,"Error allocating resoures for the client");
				close(fd); /* May be already closed, just ingore errors */
				return;
		}
		/* If maxclient directive is set and this is one client more... close the
		 * connection. Note that we create the client instead to check before
		 * for this condition, since now the socket is already set in nonblocking
		 * mode and we can send an error for free using the Kernel I/O */
		if (server.maxclients && listLength(server.clients) > server.maxclients) {
				char *err = "-ERR max number of clients reached\r\n";

				/* That's a best effort error message, don't check write errors */
				if (write(c->fd,err,strlen(err)) == -1) {
						/* Nothing to do, Just to avoid the warning... */
				}
				freeClient(c);
				return;
		}
		server.stat_numconnections++;
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
		int cport, cfd;
		char cip[128];
		REDIS_NOTUSED(el);
		REDIS_NOTUSED(mask);
		REDIS_NOTUSED(privdata);

		cfd = anetTcpAccept(server.neterr, fd, cip, &cport);
		if (cfd == AE_ERR) {
				redisLog(REDIS_WARNING,"Accepting client connection: %s", server.neterr);
				return;
		}
		redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
		acceptCommonHandler(cfd);
}

static void freeClientArgv(redisClient *c) {
		int j;
		for (j = 0; j < c->argc; j++)
				decrRefCount(c->argv[j]);
		c->argc = 0;
		c->cmd = NULL;
}


void freeClient(redisClient *c) {
		listNode *ln;

		/* If this is marked as current client unset it */
		if (server.current_client == c) server.current_client = NULL;

		/* Note that if the client we are freeing is blocked into a blocking
		 * call, we have to set querybuf to NULL *before* to call
		 * unblockClientWaitingData() to avoid processInputBuffer() will get
		 * called. Also it is important to remove the file events after
		 * this, because this call adds the READABLE event. */
		sdsfree(c->querybuf);
		c->querybuf = NULL;

		/* Obvious cleanup */
		aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
		aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
		listRelease(c->reply);
		freeClientArgv(c);
		close(c->fd);
		/* Remove from the list of clients */
		ln = listSearchKey(server.clients,c);
		redisAssert(ln != NULL);
		listDelNode(server.clients,ln);
		zfree(c->argv);
		zfree(c);
}

void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
		redisClient *c = privdata;
		int nwritten = 0, totwritten = 0, objlen;
		size_t objmem;
		robj *o;
		REDIS_NOTUSED(el);
		REDIS_NOTUSED(mask);

		while(c->bufpos > 0 || listLength(c->reply)) {
				if (c->bufpos > 0) {
						nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
						if (nwritten <= 0) break;

						c->sentlen += nwritten;
						totwritten += nwritten;

						/* If the buffer was sent, set bufpos to zero to continue with
						 * the remainder of the reply. */
						if (c->sentlen == c->bufpos) {
								c->bufpos = 0;
								c->sentlen = 0;
						}
				} else {
						o = listNodeValue(listFirst(c->reply));
						objlen = sdslen(o->ptr);
						objmem = zmalloc_size_sds(o->ptr);

						if (objlen == 0) {
								listDelNode(c->reply,listFirst(c->reply));
								continue;
						}

						nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
						if (nwritten <= 0) break;

						c->sentlen += nwritten;
						totwritten += nwritten;

						/* If we fully sent the object on head go to the next one */
						if (c->sentlen == objlen) {
								listDelNode(c->reply,listFirst(c->reply));
								c->sentlen = 0;
								c->reply_bytes -= objmem;
						}
				}
				/* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
				 * bytes, in a single threaded server it's a good idea to serve
				 * other clients as well, even if a very large request comes from
				 * super fast link that is always able to accept data (in real world
				 * scenario think about 'KEYS *' against the loopback interface).
				 *
				 * However if we are over the maxmemory limit we ignore that and
				 * just deliver as much data as it is possible to deliver. */
				if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
		}
		if (nwritten == -1) {
				if (errno == EAGAIN) {
						nwritten = 0;
				} else {
						redisLog(REDIS_VERBOSE,
										"Error writing to client: %s", strerror(errno));
						freeClient(c);
						return;
				}
		}
		if (totwritten > 0) c->lastinteraction = time(NULL);
		if (c->bufpos == 0 && listLength(c->reply) == 0) {
				c->sentlen = 0;
				aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);

				/* Close connection after entire reply has been sent. */
				if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
		}
}


void addReplyLongLong(redisClient *c, long long ll) {
		if (ll == 0)
				addReply(c,shared.czero);
		else if (ll == 1)
				addReply(c,shared.cone);
		else
				_addReplyLongLong(c,ll,':');
}
/* resetClient prepare the client to process the next command */
void resetClient(redisClient *c) {
		freeClientArgv(c);
		c->reqtype = 0;
		c->multibulklen = 0;
		c->bulklen = -1;
}

void closeTimedoutClients(void) {
		redisClient *c;
		listNode *ln;
		time_t now = time(NULL);
		listIter li;

		listRewind(server.clients,&li);
		while ((ln = listNext(&li)) != NULL) {
				c = listNodeValue(ln);
				if (server.maxidletime &&
								(now - c->lastinteraction > server.maxidletime))
				{
						redisLog(REDIS_VERBOSE,"Closing idle client");
						freeClient(c);
				}
		}
}

int processInlineBuffer(redisClient *c) {
		char *newline = strstr(c->querybuf,"\r\n");
		int argc, j;
		sds *argv;
		size_t querylen;

		/* Nothing to do without a \r\n */
		if (newline == NULL) {
				if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
						addReplyError(c,"Protocol error: too big inline request");
						setProtocolError(c,0);
				}
				return REDIS_ERR;
		}

		/* Split the input buffer up to the \r\n */
		querylen = newline-(c->querybuf);
		argv = sdssplitlen(c->querybuf,querylen," ",1,&argc);

		/* Leave data after the first line of the query in the buffer */
		c->querybuf = sdsrange(c->querybuf,querylen+2,-1);

		/* Setup argv array on client structure */
		if (c->argv) zfree(c->argv);
		c->argv = zmalloc(sizeof(robj*)*argc);

		/* Create redis objects for all arguments. */
		for (c->argc = 0, j = 0; j < argc; j++) {
				if (sdslen(argv[j])) {
						c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
						c->argc++;
				} else {
						sdsfree(argv[j]);
				}
		}
		zfree(argv);
		return REDIS_OK;
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
static void setProtocolError(redisClient *c, int pos) {
		if (server.verbosity >= REDIS_VERBOSE) {
				sds client = getClientInfoString(c);
				redisLog(REDIS_VERBOSE,
								"Protocol error from client: %s", client);
				sdsfree(client);
		}
		c->flags |= REDIS_CLOSE_AFTER_REPLY;
		c->querybuf = sdsrange(c->querybuf,pos,-1);
}

int processMultibulkBuffer(redisClient *c) {
		char *newline = NULL;
		int pos = 0, ok;
		long long ll;

		if (c->multibulklen == 0) {
				/* The client should have been reset */
				redisAssert(c->argc == 0);

				/* Multi bulk length cannot be read without a \r\n */
				newline = strchr(c->querybuf,'\r');
				if (newline == NULL) {
						if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
								addReplyError(c,"Protocol error: too big mbulk count string");
								setProtocolError(c,0);
						}
						return REDIS_ERR;
				}

				/* Buffer should also contain \n */
				if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
						return REDIS_ERR;

				/* We know for sure there is a whole line since newline != NULL,
				 * so go ahead and find out the multi bulk length. */
				redisAssert(c->querybuf[0] == '*');
				ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
				if (!ok || ll > 1024*1024) {
						addReplyError(c,"Protocol error: invalid multibulk length");
						setProtocolError(c,pos);
						return REDIS_ERR;
				}

				pos = (newline-c->querybuf)+2;
				if (ll <= 0) {
						c->querybuf = sdsrange(c->querybuf,pos,-1);
						return REDIS_OK;
				}

				c->multibulklen = ll;

				/* Setup argv array on client structure */
				if (c->argv) zfree(c->argv);
				c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
		}

		redisAssert(c->multibulklen > 0);
		while(c->multibulklen) {
				/* Read bulk length if unknown */
				if (c->bulklen == -1) {
						newline = strchr(c->querybuf+pos,'\r');
						if (newline == NULL) {
								if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
										addReplyError(c,"Protocol error: too big bulk count string");
										setProtocolError(c,0);
								}
								break;
						}

						/* Buffer should also contain \n */
						if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
								break;

						if (c->querybuf[pos] != '$') {
								addReplyErrorFormat(c,
												"Protocol error: expected '$', got '%c'",
												c->querybuf[pos]);
								setProtocolError(c,pos);
								return REDIS_ERR;
						}

						ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
						if (!ok || ll < 0 || ll > 512*1024*1024) {
								addReplyError(c,"Protocol error: invalid bulk length");
								setProtocolError(c,pos);
								return REDIS_ERR;
						}

						pos += newline-(c->querybuf+pos)+2;
						c->bulklen = ll;
				}

				/* Read bulk argument */
				if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
						/* Not enough data (+2 == trailing \r\n) */
						break;
				} else {
						c->argv[c->argc++] = createStringObject(c->querybuf+pos,c->bulklen);
						pos += c->bulklen+2;
						c->bulklen = -1;
						c->multibulklen--;
				}
		}

		/* Trim to pos */
		c->querybuf = sdsrange(c->querybuf,pos,-1);

		/* We're done when c->multibulk == 0 */
		if (c->multibulklen == 0) return REDIS_OK;

		/* Still not read to process the command */
		return REDIS_ERR;
}

void processInputBuffer(redisClient *c) {
		/* Keep processing while there is something in the input buffer */
		while(sdslen(c->querybuf)) {

				/* REDIS_CLOSE_AFTER_REPLY closes the connection once the reply is
				 * written to the client. Make sure to not let the reply grow after
				 * this flag has been set (i.e. don't process more commands). */
				if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

				/* Determine request type when unknown. */
				if (!c->reqtype) {
						if (c->querybuf[0] == '*') {
								c->reqtype = REDIS_REQ_MULTIBULK;
						} else {
								c->reqtype = REDIS_REQ_INLINE;
						}
				}

				if (c->reqtype == REDIS_REQ_INLINE) {
						if (processInlineBuffer(c) != REDIS_OK) break;
				} else if (c->reqtype == REDIS_REQ_MULTIBULK) {
						if (processMultibulkBuffer(c) != REDIS_OK) break;
				} else {
						redisPanic("Unknown request type");
				}

				/* Multibulk processing could see a <= 0 length. */
				if (c->argc == 0) {
						resetClient(c);
				} else {
						/* Only reset the client when the command was executed. */
						if (processCommand(c) == REDIS_OK)
								resetClient(c);
				}
		}
}

void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
		redisClient *c = (redisClient*) privdata;
		char buf[REDIS_IOBUF_LEN];
		int nread;
		REDIS_NOTUSED(el);
		REDIS_NOTUSED(mask);

		server.current_client = c;
		nread = read(fd, buf, REDIS_IOBUF_LEN);
		if (nread == -1) {
				if (errno == EAGAIN) {
						nread = 0;
				} else {
						redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
						freeClient(c);
						return;
				}
		} else if (nread == 0) {
				redisLog(REDIS_VERBOSE, "Client closed connection");
				freeClient(c);
				return;
		}
		if (nread) {
				c->querybuf = sdscatlen(c->querybuf,buf,nread);
				c->lastinteraction = time(NULL);
		} else {
				server.current_client = NULL;
				return;
		}
		if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
				sds ci = getClientInfoString(c), bytes = sdsempty();

				bytes = sdscatrepr(bytes,c->querybuf,64);
				redisLog(REDIS_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
				sdsfree(ci);
				sdsfree(bytes);
				freeClient(c);
				return;
		}
		processInputBuffer(c);
		server.current_client = NULL;
}

void getClientsMaxBuffers(unsigned long *longest_output_list, unsigned long *biggest_input_buffer) {
		redisClient *c;
		listNode *ln;
		listIter li;
		unsigned long lol = 0, bib = 0;

		listRewind(server.clients,&li);
		while ((ln = listNext(&li)) != NULL) {
				c = listNodeValue(ln);

				if (listLength(c->reply) > lol) lol = listLength(c->reply);
				if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);
		}
		*longest_output_list = lol;
		*biggest_input_buffer = bib;
}

/* Turn a Redis client into an sds string representing its state. */
sds getClientInfoString(redisClient *client) {
		char ip[32], flags[16], events[3], *p;
		int port;
		time_t now = time(NULL);
		int emask;

		if (anetPeerToString(client->fd,ip,&port) == -1) {
				ip[0] = '?';
				ip[1] = '\0';
				port = 0;
		}
		p = flags;

		if (client->flags & REDIS_CLOSE_AFTER_REPLY) *p++ = 'c';
		if (p == flags) *p++ = 'N';
		*p++ = '\0';

		emask = client->fd == -1 ? 0 : aeGetFileEvents(server.el,client->fd);
		p = events;
		if (emask & AE_READABLE) *p++ = 'r';
		if (emask & AE_WRITABLE) *p++ = 'w';
		*p = '\0';
		return sdscatprintf(sdsempty(),
						"addr=%s:%d fd=%d idle=%ld flags=%s db=%d qbuf=%lu obl=%lu oll=%lu events=%s cmd=%s",
						ip,port,client->fd,
						(long)(now - client->lastinteraction),
						flags,
						client->db->id,
						(unsigned long) sdslen(client->querybuf),
						(unsigned long) client->bufpos,
						(unsigned long) listLength(client->reply),
						events,
						client->lastcmd ? client->lastcmd->name : "NULL");
}

sds getAllClientsInfoString(void) {
		listNode *ln;
		listIter li;
		redisClient *client;
		sds o = sdsempty();

		listRewind(server.clients,&li);
		while ((ln = listNext(&li)) != NULL) {
				sds cs;

				client = listNodeValue(ln);
				cs = getClientInfoString(client);
				o = sdscatsds(o,cs);
				sdsfree(cs);
				o = sdscatlen(o,"\n",1);
		}
		return o;
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
void _addReplySdsToList(redisClient *c, sds s) {
		robj *tail;

		if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
				sdsfree(s);
				return;
		}

		if (listLength(c->reply) == 0) {
				listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
				c->reply_bytes += zmalloc_size_sds(s);
		} else {
				tail = listNodeValue(listLast(c->reply));

				/* Append to this object when possible. */
				if (tail->ptr != NULL &&
								sdslen(tail->ptr)+sdslen(s) <= REDIS_REPLY_CHUNK_BYTES)
				{
						c->reply_bytes -= zmalloc_size_sds(tail->ptr);
						tail = dupLastObjectIfNeeded(c->reply);
						tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
						c->reply_bytes += zmalloc_size_sds(tail->ptr);
						sdsfree(s);
				} else {
						listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
						c->reply_bytes += zmalloc_size_sds(s);
				}
		}
}

void addReplySds(redisClient *c, sds s) {
		if (_installWriteEvent(c) != REDIS_OK) {
				/* The caller expects the sds to be free'd. */
				sdsfree(s);
				return;
		}
		if (_addReplyToBuffer(c,s,sdslen(s)) == REDIS_OK) {
				sdsfree(s);
		} else {
				/* This method free's the sds when it is no longer needed. */
				_addReplySdsToList(c,s);
		}
}
