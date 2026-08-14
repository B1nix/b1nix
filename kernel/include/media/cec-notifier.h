/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_MEDIA_CEC_NOTIFIER_H
#define LKPI_MEDIA_CEC_NOTIFIER_H
#include <linux/err.h>
/* HDMI CEC. b1nix has no CEC adapter for a notifier to feed, so registration
 * fails and the driver carries on without it — which is what upstream does on a
 * kernel built without CEC. */
struct cec_notifier;
struct cec_connector_info;
static inline struct cec_notifier *
cec_notifier_conn_register(struct device *dev, const char *port_name,
                           const struct cec_connector_info *conn_info)
{ (void)dev; (void)port_name; (void)conn_info; return 0; }
static inline void cec_notifier_conn_unregister(struct cec_notifier *n) { (void)n; }
static inline void cec_notifier_set_phys_addr(struct cec_notifier *n, u16 pa)
{ (void)n; (void)pa; }
static inline void cec_notifier_phys_addr_invalidate(struct cec_notifier *n) { (void)n; }

/* Feed the notifier the physical address parsed out of an EDID, and fill in a
 * connector description for it. No adapter is ever registered (see above), so
 * both act on a notifier that does not exist. */
struct edid;
struct drm_connector;
static inline void cec_notifier_set_phys_addr_from_edid(struct cec_notifier *n,
                                                        const struct edid *edid)
{ (void)n; (void)edid; }
static inline void cec_fill_conn_info_from_drm(struct cec_connector_info *conn_info,
                                               const struct drm_connector *connector)
{ (void)conn_info; (void)connector; }


/* What a CEC adapter is told about the connector it hangs off. No adapter is
 * registered here (see above), so nothing reads it — the members are the ones
 * a driver fills in. */
struct cec_connector_info {
	u32 type;
	struct { int card_no; u32 connector_id; } drm;
};

#endif
