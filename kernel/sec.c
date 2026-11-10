#include "kernel.h"
#include "mm.h"
#include "irq.h"
#include "task.h"
#include "obj.h"
#include "sec.h"

void sec_token_system(struct token *t)
{
    memset(t, 0, sizeof(*t));
    t->user = SID_SYSTEM;
    t->privs = PRIV_BYPASS_ACL | PRIV_TAKE_OWNERSHIP;
}

void sec_token_user(struct token *t, uint32_t user, const uint32_t *groups, uint32_t n, uint32_t privs)
{
    memset(t, 0, sizeof(*t));
    t->user = user;
    t->privs = privs;
    if (n > SEC_GROUPS_MAX)
        n = SEC_GROUPS_MAX;
    for (uint32_t i = 0; i < n; i++)
        t->groups[i] = groups[i];
    t->ngroups = n;
}

static bool token_is(const struct token *t, uint32_t sid)
{
    if (t->user == sid)
        return true;
    for (uint32_t i = 0; i < t->ngroups; i++)
        if (t->groups[i] == sid)
            return true;
    return false;
}

static bool ace_applies(const struct token *t, uint32_t sid)
{
    return sid == SID_EVERYONE || token_is(t, sid);
}

uint32_t sec_map_generic(const struct obj_type *t, uint32_t m)
{
    if (!(m & ACCESS_GENERIC_MASK))
        return m;
    uint32_t out = m & ~ACCESS_GENERIC_MASK;
    const struct gmap *g = t->gmap;
    if (g) {
        if (m & ACCESS_GENERIC_READ)    out |= g->read;
        if (m & ACCESS_GENERIC_WRITE)   out |= g->write;
        if (m & ACCESS_GENERIC_EXECUTE) out |= g->exec;
        if (m & ACCESS_GENERIC_ALL)     out |= g->all;
    }
    return out;
}

int sec_check(const struct token *tok, struct object *o, uint32_t desired, uint32_t *granted)
{
    const struct obj_type *ty = o->type;
    uint32_t valid = ty->valid_access;
    bool max = desired & ACCESS_MAXIMUM;
    uint32_t want = sec_map_generic(ty, desired & ~ACCESS_MAXIMUM);

    if (want & ~valid)
        return -E_INVAL;
    if (!tok) {
        *granted = max ? valid : want;
        return 0;
    }

    uint32_t got = 0, denied = 0;
    preempt_disable();
    const struct secdesc *sd = o->sd;
    if (tok->privs & PRIV_BYPASS_ACL) {
        got = valid;
    } else if (!sd || !(sd->flags & SD_DACL_PRESENT)) {
        got = valid;
    } else {
        if (sd->owner && token_is(tok, sd->owner))
            got = ACCESS_READ_CONTROL | ACCESS_WRITE_DAC;
        for (uint32_t i = 0; i < sd->nace && i < SD_ACE_MAX; i++) {
            const struct ace *a = &sd->aces[i];
            if ((a->flags & ACE_INHERIT_ONLY) || !ace_applies(tok, a->sid))
                continue;
            uint32_t m = sec_map_generic(ty, a->mask) & valid;
            if (a->type == ACE_ALLOW)
                got |= m & ~denied;
            else
                denied |= m & ~got;
        }
    }
    if (tok->privs & PRIV_TAKE_OWNERSHIP)
        got |= ACCESS_WRITE_OWNER;
    preempt_enable();
    got &= valid;

    if (max) {
        if (!got)
            return -E_ACCES;
        *granted = got;
        return 0;
    }
    if (want & ~got)
        return -E_ACCES;
    *granted = want;
    return 0;
}

static int sd_ensure(struct object *o)
{
    if (!o->sd) {
        o->sd = kzalloc(sizeof(*o->sd));
        if (!o->sd)
            return -E_NOMEM;
    }
    return 0;
}

int sec_obj_init(struct object *o, uint32_t owner)
{
    preempt_disable();
    int rc = sd_ensure(o);
    if (!rc)
        o->sd->owner = owner;
    preempt_enable();
    return rc;
}

int sec_kset(struct object *o, uint32_t owner, const struct ace *aces, int n)
{
    if (n > SD_ACE_MAX)
        return -E_INVAL;
    preempt_disable();
    int rc = sd_ensure(o);
    if (!rc) {
        struct secdesc *sd = o->sd;
        sd->owner = owner;
        if (n < 0) {
            sd->flags &= ~SD_DACL_PRESENT;
            sd->nace = 0;
        } else {
            sd->flags |= SD_DACL_PRESENT;
            sd->nace = (uint32_t)n;
            memset(sd->aces, 0, sizeof(sd->aces));
            for (int i = 0; i < n; i++)
                sd->aces[i] = aces[i];
        }
    }
    preempt_enable();
    return rc;
}

int sec_inherit(struct object *child, const struct object *parent, const struct token *tok)
{
    int rc = sd_ensure(child);
    if (rc)
        return rc;
    struct secdesc *sd = child->sd;
    if (!sd->owner && tok)
        sd->owner = tok->user;
    if (sd->flags & SD_DACL_PRESENT)
        return 0;

    bool isdir = child->type->id == OBJ_DIR;
    const struct secdesc *ps = parent->sd;
    uint32_t n = 0;
    if (ps && (ps->flags & SD_DACL_PRESENT)) {
        for (uint32_t i = 0; i < ps->nace && i < SD_ACE_MAX; i++) {
            const struct ace *a = &ps->aces[i];
            if (!(a->flags & ACE_INHERIT))
                continue;
            struct ace *d = &sd->aces[n++];
            *d = *a;
            if (d->sid == SID_CREATOR_OWNER)
                d->sid = sd->owner;
            d->flags = isdir ? ACE_INHERIT : 0;
        }
    }
    if (!n) {
        sd->aces[0] = (struct ace){ ACE_ALLOW, 0, 0, sd->owner, ACCESS_GENERIC_ALL };
        n = 1;
    }
    sd->nace = n;
    sd->flags |= SD_DACL_PRESENT;
    return 0;
}

static int sd_validate(const struct object *o, const struct secdesc *in, uint32_t which)
{
    if (which & SEC_DACL) {
        if (in->flags & ~SD_DACL_PRESENT)
            return -E_INVAL;
        if (in->nace > SD_ACE_MAX)
            return -E_INVAL;
        if (!(in->flags & SD_DACL_PRESENT) && in->nace)
            return -E_INVAL;
        uint32_t ok = o->type->valid_access | ACCESS_GENERIC_MASK;
        for (uint32_t i = 0; i < in->nace; i++) {
            const struct ace *a = &in->aces[i];
            if (a->type > ACE_DENY || (a->flags & ~(ACE_INHERIT | ACE_INHERIT_ONLY)) || a->rsvd || !a->sid)
                return -E_INVAL;
            if ((a->flags & ACE_INHERIT_ONLY) && !(a->flags & ACE_INHERIT))
                return -E_INVAL;
            if (a->mask & ~ok)
                return -E_INVAL;
        }
    }
    return 0;
}

long sys_sec_dispatch(struct task *t, uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    struct object *o = NULL;
    struct secdesc sd;
    long rc;

    switch (nr) {
    case SYS_TOKEN_INFO:
        return user_copy_out(t, a, &t->tok, sizeof(t->tok));

    case SYS_OBJ_GETSEC:
        rc = ht_lookup(t->ht, (uint32_t)a, NULL, ACCESS_READ_CONTROL, &o, NULL);
        if (rc)
            return rc;
        memset(&sd, 0, sizeof(sd));
        preempt_disable();
        if (o->sd)
            sd = *o->sd;
        preempt_enable();
        obj_unref(o);
        return user_copy_out(t, b, &sd, sizeof(sd));

    case SYS_OBJ_SETSEC: {
        uint32_t which = (uint32_t)c;
        if (!which || (which & ~(SEC_OWNER | SEC_DACL)))
            return -E_INVAL;
        rc = user_copy_in(t, &sd, b, sizeof(sd));
        if (rc)
            return rc;
        uint32_t need = ((which & SEC_OWNER) ? ACCESS_WRITE_OWNER : 0) |
                        ((which & SEC_DACL) ? ACCESS_WRITE_DAC : 0);
        rc = ht_lookup(t->ht, (uint32_t)a, NULL, need, &o, NULL);
        if (rc)
            return rc;
        rc = sd_validate(o, &sd, which);
        if (!rc && (which & SEC_OWNER)) {
            if (!sd.owner)
                rc = -E_INVAL;
            else if (!(t->tok.privs & PRIV_BYPASS_ACL) && !token_is(&t->tok, sd.owner))
                rc = -E_PERM;
        }
        if (!rc) {
            preempt_disable();
            rc = sd_ensure(o);
            if (!rc) {
                if (which & SEC_OWNER)
                    o->sd->owner = sd.owner;
                if (which & SEC_DACL) {
                    o->sd->flags = (o->sd->flags & ~SD_DACL_PRESENT) | (sd.flags & SD_DACL_PRESENT);
                    o->sd->nace = sd.nace;
                    memset(o->sd->aces, 0, sizeof(o->sd->aces));
                    for (uint32_t i = 0; i < sd.nace; i++)
                        o->sd->aces[i] = sd.aces[i];
                }
            }
            preempt_enable();
        }
        obj_unref(o);
        return rc;
    }
    }
    return -E_NOSYS;
}
