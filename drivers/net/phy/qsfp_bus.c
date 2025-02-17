// SPDX-License-Identifier: GPL-2.0-only
#include <linux/export.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/phylink.h>
#include <linux/property.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>

#include <linux/qsfp.h>
#include <linux/of_platform.h>

/**
 * struct qsfp_bus - internal representation of a qsfp bus
 */
struct qsfp_bus {
	/* private: */
	struct kref kref;
	struct list_head node;
	struct fwnode_handle *fwnode;

	const struct qsfp_socket_ops *socket_ops;
	struct device *qsfp_dev;
	struct qsfp *qsfp;
	const struct qsfp_quirk *qsfp_quirk;

	const struct qsfp_upstream_ops *upstream_ops;
	void *upstream;
	struct phy_device *phydev;

	bool registered;
	bool started;
};

/**
 * qsfp_parse_port() - Parse the EEPROM base ID, setting the port type
 * @bus: a pointer to the &struct sfp_bus structure for the sfp module
 * @id: a pointer to the module's &struct sfp_eeprom_id
 * @support: optional pointer to an array of unsigned long for the
 *   ethtool support mask
 *
 * Parse the EEPROM identification given in @id, and return one of
 * %PORT_TP, %PORT_FIBRE or %PORT_OTHER. If @support is non-%NULL,
 * also set the ethtool %ETHTOOL_LINK_MODE_xxx_BIT corresponding with
 * the connector type.
 *
 * If the port type is not known, returns %PORT_OTHER.
 */
int qsfp_parse_port(struct qsfp_bus *bus, const struct qsfp_eeprom_id *id,
		    unsigned long *support)
{
	int port;

	/* port is the physical connector, set this from the connector field. */
	switch (id->base.etile_qsfp_connector_type) {
	case SFF8024_QSFP_DD_CONNECTOR_SC:
	case SFF8024_QSFP_DD_CONNECTOR_FIBERJACK:
	case SFF8024_QSFP_DD_CONNECTOR_LC:
	case SFF8024_QSFP_DD_CONNECTOR_MT_RJ:
	case SFF8024_QSFP_DD_CONNECTOR_MU:
	case SFF8024_QSFP_DD_CONNECTOR_OPTICAL_PIGTAIL:
	case SFF8024_QSFP_DD_CONNECTOR_MPO_1X12:
	case SFF8024_QSFP_DD_CONNECTOR_MPO_2X16:
		port = PORT_FIBRE;
		break;

	case SFF8024_QSFP_DD_CONNECTOR_RJ45:
		port = PORT_TP;
		break;

	case SFF8024_QSFP_DD_CONNECTOR_COPPER_PIGTAIL:
		port = PORT_DA;
		break;

	case SFF8024_QSFP_DD_CONNECTOR_UNSPEC:
		port = PORT_TP;
		break;

	case SFF8024_QSFP_DD_CONNECTOR_SG: /* guess */
	case SFF8024_QSFP_DD_CONNECTOR_HSSDC_II:
	case SFF8024_QSFP_DD_CONNECTOR_NOSEPARATE:
	case SFF8024_QSFP_DD_CONNECTOR_MXC_2X16:
		/*supporting connector type with extended
		 *spec for both electrical and optical interface
		 */
		if (id->base.etile_qsfp_ext_spec_compliance &
			SFF8024_QSFP_ECC_100G_25GAUI_C2M_AOC_LOW_BER) {
			port = PORT_AUI;
			break;
		}
		port = PORT_OTHER;
		break;
	default:
		dev_warn(bus->qsfp_dev, "QSFP: unknown connector id 0x%02x\n",
			 id->base.etile_qsfp_connector_type);
		port = PORT_OTHER;
		break;
	}

	if (support) {
		switch (port) {
		case PORT_FIBRE:
			phylink_set(support, FIBRE);
			break;

		case PORT_TP:
			phylink_set(support, TP);
			break;

		/*added support to AUI(Attachment Unit Interface) port*/
		case PORT_AUI:
			phylink_set(support, AUI);
			break;
		}
	}

	return port;
}
EXPORT_SYMBOL_GPL(qsfp_parse_port);

/**
 * qsfp_may_have_phy() - indicate whether the module may have a PHY
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 * @id: a pointer to the module's &struct qsfp_eeprom_id
 *
 * Parse the EEPROM identification given in @id, and return whether
 * this module may have a PHY.
 */
bool qsfp_may_have_phy(struct qsfp_bus *bus, const struct qsfp_eeprom_id *id)
{
	if (id->base.etile_qsfp_identifier != SFF8024_ID_QSFP_DD_INF_8628) {
		switch (id->base.etile_qsfp_spec_compliance_1[0]) {
		case SFF8636_QSFP_ECC_40G_ACTIVE_CABLE:
		case SFF8636_QSFP_ECC_40GBASE_LR4:
		case SFF8636_QSFP_ECC_40GBASE_SR4:
		case SFF8636_QSFP_ECC_40GBASE_CR4:
		case SFF8636_QSFP_ECC_10GBASE_SR:
		case SFF8636_QSFP_ECC_10GBASE_LR:
		case SFF8636_QSFP_ECC_10GBASE_LRM:
		case SFF8636_QSFP_ECC_EXTENDED:
			return true;
		default:
			break;
		}
	}

	return false;
}
EXPORT_SYMBOL_GPL(qsfp_may_have_phy);

/**
 * qsfp_parse_support() - Parse the eeprom id for supported link modes
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 * @id: a pointer to the module's &struct qsfp_eeprom_id
 * @support: pointer to an array of unsigned long for the ethtool support mask
 * @interfaces: pointer to an array of unsigned long for phy interface modes
 *		mask
 *
 * Parse the EEPROM identification information and derive the supported
 * ethtool link modes for the module.
 */
void qsfp_parse_support(struct qsfp_bus *bus, const struct qsfp_eeprom_id *id,
			unsigned long *support, unsigned long *interfaces)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(modes) = { 0, };

	/* Set ethtool support from the compliance fields. */
	if (id->base.etile_qsfp_spec_compliance_1[0] & SFF8636_QSFP_ECC_10GBASE_SR) {
		phylink_set(modes, 10000baseSR_Full);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, interfaces);
	}

	if (id->base.etile_qsfp_spec_compliance_1[0] & SFF8636_QSFP_ECC_10GBASE_LR) {
		phylink_set(modes, 10000baseSR_Full);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, interfaces);
	}

	if (id->base.etile_qsfp_spec_compliance_1[0] & SFF8636_QSFP_ECC_10GBASE_LRM) {
		phylink_set(modes, 10000baseLRM_Full);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, interfaces);
	}

	if ((id->base.etile_qsfp_spec_compliance_1[3] & SFF8024_QSFP_SCC_1000BASE_SX) ||
	    (id->base.etile_qsfp_spec_compliance_1[3] & SFF8024_QSFP_SCC_1000BASE_LX) ||
	    (id->base.etile_qsfp_spec_compliance_1[3] & SFF8024_QSFP_SCC_1000BASE_CX)) {
		phylink_set(modes, 1000baseX_Full);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX, interfaces);
	}

	if (id->base.etile_qsfp_spec_compliance_1[3] & SFF8024_QSFP_SCC_1000BASE_T) {
		phylink_set(modes, 1000baseT_Half);
		phylink_set(modes, 1000baseT_Full);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX, interfaces);
		__set_bit(PHY_INTERFACE_MODE_SGMII, interfaces);
	}

	switch (id->base.etile_qsfp_ext_spec_compliance) {
	case SFF8024_QSFP_ECC_UNSPEC:
		phylink_set(modes, 25000baseKR_Full);
		__set_bit(PHY_INTERFACE_MODE_25GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_100GBASE_SR4_25GBASE_SR:
		phylink_set(modes, 100000baseSR4_Full);
		phylink_set(modes, 25000baseSR_Full);
		__set_bit(PHY_INTERFACE_MODE_25GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_100GBASE_LR4_25GBASE_LR:
	case SFF8024_QSFP_ECC_100GBASE_ER4_25GBASE_ER:
		phylink_set(modes, 100000baseLR4_ER4_Full);
		break;
	case SFF8024_QSFP_ECC_100GBASE_CR4:
		phylink_set(modes, 100000baseCR4_Full);
		fallthrough;
	case SFF8024_QSFP_ECC_25GBASE_CR_S:
	case SFF8024_QSFP_ECC_25GBASE_CR_N:
		phylink_set(modes, 25000baseCR_Full);
		__set_bit(PHY_INTERFACE_MODE_25GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_10GBASE_T_SFI:
	case SFF8024_QSFP_ECC_10GBASE_T_SR:
		phylink_set(modes, 10000baseT_Full);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_5GBASE_T:
		phylink_set(modes, 5000baseT_Full);
		__set_bit(PHY_INTERFACE_MODE_5GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_2_5GBASE_T:
		phylink_set(modes, 2500baseT_Full);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, interfaces);
		break;
	case SFF8024_QSFP_ECC_100G_25GAUI_C2M_AOC_LOW_BER:
		phylink_set(modes, 100000baseKR4_Full);
		phylink_set(modes, 25000baseKR_Full);
		__set_bit(PHY_INTERFACE_MODE_25GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_100GBASE_SR10:
		phylink_set(modes, 100000baseSR4_Full);
		break;
	case SFF8024_QSFP_ECC_100G_25GAUI_C2M_AOC:
		phylink_set(modes, 100000baseSR4_Full);
		phylink_set(modes, 25000baseSR_Full);
		__set_bit(PHY_INTERFACE_MODE_25GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_100G_CWDM4:
		phylink_set(modes, 100000baseCR4_Full);
		break;
	case SFF8024_QSFP_ECC_100G_PSM4:
		phylink_set(modes, 100000baseCR4_Full);
		break;
	case SFF8024_QSFP_ECC_10M:
		phylink_set(modes, 10baseT_Full);
		break;
	case SFF8024_QSFP_ECC_40GBASE_ER:
		phylink_set(modes, 40000baseLR4_Full);
		break;
	case SFF8024_QSFP_ECC_10GBASE_SR:
		phylink_set(modes, 10000baseSR_Full);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, interfaces);
		break;
	case SFF8024_QSFP_ECC_100G_CLR4:
		phylink_set(modes, 100000baseLR4_ER4_Full);
		break;
	case SFF8024_QSFP_ECC_100G_ACC_25G_ACC:
		phylink_set(modes, 100000baseCR4_Full);
		phylink_set(modes, 25000baseCR_Full);
		__set_bit(PHY_INTERFACE_MODE_25GBASER, interfaces);
		break;
	default:
		dev_warn(bus->qsfp_dev,
			 "Unknown/unsupported extended compliance code: 0x%02x\n",
			 id->base.etile_qsfp_ext_spec_compliance);
		break;
	}

	/* For fibre channel QSFP, derive possible BaseX modes */
	if ((id->base.etile_qsfp_spec_compliance_1[7] & SFF8024_QSFP_SCC_FC_SPEED_100) ||
	    (id->base.etile_qsfp_spec_compliance_1[7] & SFF8024_QSFP_SCC_FC_SPEED_200) ||
	    (id->base.etile_qsfp_spec_compliance_1[7] & SFF8024_QSFP_SCC_FC_SPEED_400)) {
		phylink_set(modes, 2500baseX_Full);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, interfaces);
	}

	/* If we haven't discovered any modes that this module supports, try
	 * the bitrate to determine supported modes. Some BiDi modules (eg,
	 * 1310nm/1550nm) are not 1000BASE-BX compliant due to the differing
	 * wavelengths, so do not set any transceiver bits.
	 *
	 * Do the same for modules supporting 2500BASE-X. Note that some
	 * modules use 2500Mbaud rather than 3100 or 3200Mbaud for
	 * 2500BASE-X, so we allow some slack here.
	 */
	//if (bitmap_empty(modes, __ETHTOOL_LINK_MODE_MASK_NBITS))
	//if (bus->qsfp_quirk && bus->qsfp_quirk->modes)
	//	bus->qsfp_quirk->modes(id, modes, interfaces);

	linkmode_or(support, support, modes);
	//bitmap_or(support, support, modes, __ETHTOOL_LINK_MODE_MASK_NBITS);

	phylink_set(support, Autoneg);
	phylink_set(support, Pause);
	phylink_set(support, Asym_Pause);
}
EXPORT_SYMBOL_GPL(qsfp_parse_support);

/**
 * qsfp_select_interface() - Select appropriate phy_interface_t mode
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 * @link_modes: ethtool link modes mask
 *
 * Derive the phy_interface_t mode for the QSFP module from the link
 * modes mask.
 */
phy_interface_t qsfp_select_interface(struct qsfp_bus *bus,
				      unsigned long *link_modes)
{
	if (phylink_test(link_modes, 25000baseCR_Full) ||
	    phylink_test(link_modes, 25000baseKR_Full) ||
	    phylink_test(link_modes, 25000baseSR_Full))
		return PHY_INTERFACE_MODE_25GBASER;

	if (phylink_test(link_modes, 10000baseCR_Full) ||
	    phylink_test(link_modes, 10000baseSR_Full) ||
	    phylink_test(link_modes, 10000baseLR_Full) ||
	    phylink_test(link_modes, 10000baseLRM_Full) ||
	    phylink_test(link_modes, 10000baseER_Full) ||
	    phylink_test(link_modes, 10000baseT_Full))
		return PHY_INTERFACE_MODE_10GBASER;

	if (phylink_test(link_modes, 5000baseT_Full))
		return PHY_INTERFACE_MODE_5GBASER;

	if (phylink_test(link_modes, 2500baseX_Full))
		return PHY_INTERFACE_MODE_2500BASEX;

	if (phylink_test(link_modes, 1000baseT_Half) ||
	    phylink_test(link_modes, 1000baseT_Full))
		return PHY_INTERFACE_MODE_SGMII;

	if (phylink_test(link_modes, 1000baseX_Full))
		return PHY_INTERFACE_MODE_1000BASEX;

	if (phylink_test(link_modes, 100baseFX_Full))
		return PHY_INTERFACE_MODE_100BASEX;

	return PHY_INTERFACE_MODE_NA;
}
EXPORT_SYMBOL_GPL(qsfp_select_interface);

static LIST_HEAD(qsfp_buses);
static DEFINE_MUTEX(qsfp_mutex);

static const struct qsfp_upstream_ops *qsfp_get_upstream_ops(struct qsfp_bus *bus)
{
	return bus->registered ? bus->upstream_ops : NULL;
}

static struct qsfp_bus *qsfp_bus_get(struct fwnode_handle *fwnode)
{
	struct qsfp_bus *qsfp, *new, *found = NULL;

	new = kzalloc(sizeof(*new), GFP_KERNEL);

	mutex_lock(&qsfp_mutex);

	list_for_each_entry(qsfp, &qsfp_buses, node) {
		if (qsfp->fwnode == fwnode) {
			kref_get(&qsfp->kref);
			found = qsfp;
			break;
		}
	}

	if (!found && new) {
		kref_init(&new->kref);
		new->fwnode = fwnode;
		list_add(&new->node, &qsfp_buses);
		found = new;
		new = NULL;
	}

	mutex_unlock(&qsfp_mutex);

	kfree(new);

	return found;
}

static void qsfp_bus_release(struct kref *kref)
{
	struct qsfp_bus *bus = container_of(kref, struct qsfp_bus, kref);

	list_del(&bus->node);
	mutex_unlock(&qsfp_mutex);
	kfree(bus);
}

/**
 * qsfp_bus_put() - put a reference on the &struct qsfp_bus
 * @bus: the &struct qsfp_bus found via qsfp_bus_find_fwnode()
 *
 * Put a reference on the &struct qsfp_bus and free the underlying structure
 * if this was the last reference.
 */
void qsfp_bus_put(struct qsfp_bus *bus)
{
	if (bus)
		kref_put_mutex(&bus->kref, qsfp_bus_release, &qsfp_mutex);
}
EXPORT_SYMBOL_GPL(qsfp_bus_put);

static int qsfp_register_bus(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = bus->upstream_ops;
	int ret;

	if (ops) {
		if (ops->link_down)
			ops->link_down(bus->upstream);
		if (ops->connect_phy && bus->phydev) {
			ret = ops->connect_phy(bus->upstream, bus->phydev);
			if (ret)
				return ret;
		}
	}
	bus->registered = true;
	bus->socket_ops->attach(bus->qsfp);
	if (bus->started)
		bus->socket_ops->start(bus->qsfp);
	if (ops)
		bus->upstream_ops->attach(bus->upstream, bus);
	return 0;
}

static void qsfp_unregister_bus(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = bus->upstream_ops;

	if (bus->registered) {
		bus->upstream_ops->detach(bus->upstream, bus);
		if (bus->started)
			bus->socket_ops->stop(bus->qsfp);
		bus->socket_ops->detach(bus->qsfp);
		if (bus->phydev && ops && ops->disconnect_phy)
			ops->disconnect_phy(bus->upstream);
	}
	bus->registered = false;
}

/**
 * qsfp_get_module_info() - Get the ethtool_modinfo for a QSFP module
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 * @modinfo: a &struct ethtool_modinfo
 *
 * Fill in the type and eeprom_len parameters in @modinfo for a module on
 * the sfp bus specified by @bus.
 *
 * Returns 0 on success or a negative errno number.
 */
int qsfp_get_module_info(struct qsfp_bus *bus, struct ethtool_modinfo *modinfo)
{
	return bus->socket_ops->module_info(bus->qsfp, modinfo);
}
EXPORT_SYMBOL_GPL(qsfp_get_module_info);

/**
 * qsfp_get_module_eeprom() - Read the QSFP module EEPROM
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 * @ee: a &struct ethtool_eeprom
 * @data: buffer to contain the EEPROM data (must be at least @ee->len bytes)
 *
 * Read the EEPROM as specified by the supplied @ee. See the documentation
 * for &struct ethtool_eeprom for the region to be read.
 *
 * Returns 0 on success or a negative errno number.
 */
int qsfp_get_module_eeprom(struct qsfp_bus *bus, struct ethtool_eeprom *ee,
			   u8 *data)
{
	return bus->socket_ops->module_eeprom(bus->qsfp, ee, data);
}
EXPORT_SYMBOL_GPL(qsfp_get_module_eeprom);

/**
 * qsfp_upstream_start() - Inform the QSFP that the network device is up
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 *
 * Inform the QSFP socket that the network device is now up, so that the
 * module can be enabled by allowing TX_DISABLE to be deasserted. This
 * should be called from the network device driver's &struct net_device_ops
 * ndo_open() method.
 */
void qsfp_upstream_start(struct qsfp_bus *bus)
{
	if (bus->registered)
		bus->socket_ops->start(bus->qsfp);
	bus->started = true;
}
EXPORT_SYMBOL_GPL(qsfp_upstream_start);

/**
 * qsfp_upstream_stop() - Inform the QSFP that the network device is down
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 *
 * Inform the QSFP socket that the network device is now up, so that the
 * module can be disabled by asserting TX_DISABLE, disabling the laser
 * in optical modules. This should be called from the network device
 * driver's &struct net_device_ops ndo_stop() method.
 */
void qsfp_upstream_stop(struct qsfp_bus *bus)
{
	if (bus->registered)
		bus->socket_ops->stop(bus->qsfp);
	bus->started = false;
}
EXPORT_SYMBOL_GPL(qsfp_upstream_stop);

static void qsfp_upstream_clear(struct qsfp_bus *bus)
{
	bus->upstream_ops = NULL;
	bus->upstream = NULL;
}

/**
 * qsfp_bus_find_fwnode() - parse and locate the QSFP bus from fwnode
 * @fwnode: firmware node for the parent device (MAC or PHY)
 *
 * Parse the parent device's firmware node for a QSFP bus, and locate
 * the qsfp_bus structure, incrementing its reference count.  This must
 * be put via qsfp_bus_put() when done.
 *
 * Returns:
 *	- on success, a pointer to the sfp_bus structure,
 *	- %NULL if no QSFP is specified,
 *	- on failure, an error pointer value:
 *
 *	- corresponding to the errors detailed for
 *	  fwnode_property_get_reference_args().
 *	- %-ENOMEM if we failed to allocate the bus.
 *	- an error from the upstream's connect_phy() method.
 */
struct qsfp_bus *qsfp_bus_find_fwnode(struct fwnode_handle *fwnode)
{
	struct fwnode_reference_args ref;
	struct qsfp_bus *bus;
	int ret;

	ret = fwnode_property_get_reference_args(fwnode, "qsfp", NULL,
						 0, 0, &ref);
	if (ret == -ENOENT)
		return NULL;
	else if (ret < 0)
		return ERR_PTR(ret);

	if (!fwnode_device_is_available(ref.fwnode)) {
		fwnode_handle_put(ref.fwnode);
		return NULL;
	}

	bus = qsfp_bus_get(ref.fwnode);
	fwnode_handle_put(ref.fwnode);
	if (!bus)
		return ERR_PTR(-ENOMEM);

	return bus;
}
EXPORT_SYMBOL_GPL(qsfp_bus_find_fwnode);

/**
 * qsfp_bus_add_upstream() - parse and register the neighbouring device
 * @bus: the &struct qsfp_bus found via qsfp_bus_find_fwnode()
 * @upstream: the upstream private data
 * @ops: the upstream's &struct qsfp_upstream_ops
 *
 * Add upstream driver for the QSFP bus, and if the bus is complete, register
 * the QSFP bus using qsfp_register_upstream().  This takes a reference on the
 * bus, so it is safe to put the bus after this call.
 *
 * Returns:
 *	- on success, a pointer to the qsfp_bus structure,
 *	- %NULL if no QSFP is specified,
 *	- on failure, an error pointer value:
 *
 *	- corresponding to the errors detailed for
 *	  fwnode_property_get_reference_args().
 *	- %-ENOMEM if we failed to allocate the bus.
 *	- an error from the upstream's connect_phy() method.
 */
int qsfp_bus_add_upstream(struct qsfp_bus *bus, void *upstream,
			  const struct qsfp_upstream_ops *ops)
{
	int ret;

	/* If no bus, return success */
	if (!bus)
		return 0;

	rtnl_lock();
	kref_get(&bus->kref);
	bus->upstream_ops = ops;
	bus->upstream = upstream;

	if (bus->qsfp) {
		ret = qsfp_register_bus(bus);
		if (ret)
			qsfp_upstream_clear(bus);
	} else {
		ret = 0;
	}
	rtnl_unlock();

	if (ret)
		qsfp_bus_put(bus);

	return ret;
}
EXPORT_SYMBOL_GPL(qsfp_bus_add_upstream);

/**
 * qsfp_bus_del_upstream() - Delete a qsfp bus
 * @bus: a pointer to the &struct qsfp_bus structure for the qsfp module
 *
 * Delete a previously registered upstream connection for the QSFP
 * module. @bus should have been added by sfp_bus_add_upstream().
 */
void qsfp_bus_del_upstream(struct qsfp_bus *bus)
{
	if (bus) {
		rtnl_lock();
		if (bus->qsfp)
			qsfp_unregister_bus(bus);
		qsfp_upstream_clear(bus);
		rtnl_unlock();

		qsfp_bus_put(bus);
	}
}
EXPORT_SYMBOL_GPL(qsfp_bus_del_upstream);

/* Socket driver entry points */
int qsfp_add_phy(struct qsfp_bus *bus, struct phy_device *phydev)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);
	int ret = 0;

	if (ops && ops->connect_phy)
		ret = ops->connect_phy(bus->upstream, phydev);

	if (ret == 0)
		bus->phydev = phydev;

	return ret;
}
EXPORT_SYMBOL_GPL(qsfp_add_phy);

void qsfp_remove_phy(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);

	if (ops && ops->disconnect_phy)
		ops->disconnect_phy(bus->upstream);
	bus->phydev = NULL;
}
EXPORT_SYMBOL_GPL(qsfp_remove_phy);

void qsfp_link_up(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);

	if (ops && ops->link_up)
		ops->link_up(bus->upstream);
}
EXPORT_SYMBOL_GPL(qsfp_link_up);

void qsfp_link_down(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);

	if (ops && ops->link_down)
		ops->link_down(bus->upstream);
}
EXPORT_SYMBOL_GPL(qsfp_link_down);

int qsfp_module_insert(struct qsfp_bus *bus, const struct qsfp_eeprom_id *id,
		       const struct qsfp_quirk *quirk)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);
	int ret = 0;

	bus->qsfp_quirk = quirk;

	if (ops && ops->module_insert)
		ret = ops->module_insert(bus->upstream, id);

	return ret;
}
EXPORT_SYMBOL_GPL(qsfp_module_insert);

void qsfp_module_remove(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);

	if (ops && ops->module_remove)
		ops->module_remove(bus->upstream);

	bus->qsfp_quirk = NULL;
}
EXPORT_SYMBOL_GPL(qsfp_module_remove);

int qsfp_module_start(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);
	int ret = 0;

	if (ops && ops->module_start)
		ret = ops->module_start(bus->upstream);

	return ret;
}
EXPORT_SYMBOL_GPL(qsfp_module_start);

void qsfp_module_stop(struct qsfp_bus *bus)
{
	const struct qsfp_upstream_ops *ops = qsfp_get_upstream_ops(bus);

	if (ops && ops->module_stop)
		ops->module_stop(bus->upstream);
}
EXPORT_SYMBOL_GPL(qsfp_module_stop);

static void qsfp_socket_clear(struct qsfp_bus *bus)
{
	bus->qsfp_dev = NULL;
	bus->qsfp = NULL;
	bus->socket_ops = NULL;
}

struct qsfp_bus *qsfp_register_socket(struct device *dev, struct qsfp *qsfp,
				      const struct qsfp_socket_ops *ops)
{
	struct qsfp_bus *bus = qsfp_bus_get(dev->fwnode);
	int ret = 0;

	if (bus) {
		rtnl_lock();
		bus->qsfp_dev = dev;
		bus->qsfp = qsfp;
		bus->socket_ops = ops;

		if (bus->upstream_ops) {
			ret = qsfp_register_bus(bus);
			if (ret)
				qsfp_socket_clear(bus);
		}
		rtnl_unlock();
	}

	if (ret) {
		qsfp_bus_put(bus);
		bus = NULL;
	}

	return bus;
}
EXPORT_SYMBOL_GPL(qsfp_register_socket);

void qsfp_unregister_socket(struct qsfp_bus *bus)
{
	rtnl_lock();
	if (bus->upstream_ops)
		qsfp_unregister_bus(bus);
	qsfp_socket_clear(bus);
	rtnl_unlock();

	qsfp_bus_put(bus);
}
EXPORT_SYMBOL_GPL(qsfp_unregister_socket);
