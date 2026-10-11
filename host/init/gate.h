/* host/init/gate.h: the Linux gate's password (docs/linux-gate.md).
 *
 * Shared by arctic-init, which prepares the protected folder this state
 * lives in, and arctic-lxss, which asks it who may skip the consent window. */
#ifndef ARCTIC_GATE_H
#define ARCTIC_GATE_H

/* root's own folder (tmpfs, 0700): the locks of arctic-lxss, and the real
 * folder on C: ("Де живе пароль") bound inside it at store, right after C:
 * is mounted and before wineserver starts. C: is mounted for uid nt, so the
 * bound folder itself belongs to nt whatever its mode says: it is the 0700
 * folder of root around it that no Windows process (all of them uid nt) can
 * pass. */
#define GATE_RUN_DIR "/run/arctic/gate"
#define GATE_STORE GATE_RUN_DIR "/store"

struct gate_state {
    int password_on;
    char hash[256];  /* $y$... (yescrypt, libxcrypt); empty only if hashing failed */
    char distro[64]; /* the distribution that is the gate to the host: arctic-build */
};

/* arctic-init only. Returns 0 once GATE_RUN_DIR is the real Gate folder and
 * the original path on C: is covered by an empty, read-only tmpfs; -1 on
 * failure (logged by the caller), in which case the gate still works but its
 * state does not survive a reboot. */
int gate_prepare_state_dir(void);

/* Reads gate.conf from GATE_RUN_DIR, or makes it (password "0000", on) when
 * there is none yet or it has no usable hash. */
void gate_load(struct gate_state *st);
int gate_save(const struct gate_state *st);

int gate_check_password(const struct gate_state *st, const char *attempt);
int gate_set_password(struct gate_state *st, const char *new_password);

#endif
