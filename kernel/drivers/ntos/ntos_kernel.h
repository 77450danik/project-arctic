/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ntos: what the kernel's side of the executive (main.c, crt.c) gives the
 * server's code, and what each gives the other
 */
#ifndef NTOS_KERNEL_H
#define NTOS_KERNEL_H

struct path;
struct cred;

/* crt.c */
void nt_crt_init(const struct path *dir);
void nt_crt_set_environ(char **env, unsigned int count);
void nt_poll_forget(int fd);		/* main.c: a descriptor closed, its watch goes */

/* main.c */
const struct cred *nt_creds(void);	/* the credentials NT runs with (arctic-init's nt user) */
size_t ntos_process_tree(int want, char *reply, size_t size);	/* ps/process.c */
struct sock;
struct process *ntos_process_of_socket(struct sock *peer);	/* ps/process.c: not started yet, its socket's peer */

/* handlers.c: the request handlers, NULL for USER's (win32k's) */
typedef void (*ntos_handler)(const void *req, void *reply);
extern const ntos_handler ntos_handlers[];

#endif
