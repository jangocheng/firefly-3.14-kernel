/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:Mark Yao <mark.yao@rock-chips.com>
 *
 * based on exynos_drm_drv.c
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/dma-iommu.h>

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_sync_helper.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/of_graph.h>
#include <linux/component.h>
#include <linux/fence.h>

#include <drm/rockchip_drm.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_fbdev.h"
#include "rockchip_drm_gem.h"

//#define DRIVER_NAME	"mali_drm"
#define DRIVER_NAME	"rockchip"
#define DRIVER_DESC	"RockChip Soc DRM"
#define DRIVER_DATE	"20140818"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

/*
 * Attach a (component) device to the shared drm dma mapping from master drm
 * device.  This is used by the VOPs to map GEM buffers to a common DMA
 * mapping.
 */
int rockchip_drm_dma_attach_device(struct drm_device *drm_dev,
				   struct device *dev)
{
	struct dma_iommu_mapping *mapping = drm_dev->dev->archdata.mapping;
	int ret;

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	dma_set_max_seg_size(dev, DMA_BIT_MASK(32));

	return arm_iommu_attach_device(dev, mapping);
}

void rockchip_drm_dma_detach_device(struct drm_device *drm_dev,
				    struct device *dev)
{
	arm_iommu_detach_device(dev);
}

static int rockchip_drm_load(struct drm_device *drm_dev, unsigned long flags)
{
	struct rockchip_drm_private *private;
	struct dma_iommu_mapping *mapping;
	struct device *dev = drm_dev->dev;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	int ret;

	private = devm_kzalloc(drm_dev->dev, sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	drm_dev->dev_private = private;

#ifdef CONFIG_DRM_DMA_SYNC
	private->cpu_fence_context = fence_context_alloc(1);
	atomic_set(&private->cpu_fence_seqno, 0);
#endif

	drm_mode_config_init(drm_dev);

	rockchip_drm_mode_config_init(drm_dev);

	dev->dma_parms = devm_kzalloc(dev, sizeof(*dev->dma_parms),
				      GFP_KERNEL);
	if (!dev->dma_parms) {
		ret = -ENOMEM;
		goto err_config_cleanup;
	}

	/* TODO(djkurtz): fetch the mapping start/size from somewhere */
	mapping = arm_iommu_create_mapping(&platform_bus_type, 0x00000000,
					   SZ_2G);
	if (IS_ERR(mapping)) {
		ret = PTR_ERR(mapping);
		goto err_config_cleanup;
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_release_mapping;

	dma_set_max_seg_size(dev, DMA_BIT_MASK(32));

	ret = arm_iommu_attach_device(dev, mapping);
	if (ret)
		goto err_release_mapping;

	/* Try to bind all sub drivers. */
	ret = component_bind_all(dev, drm_dev);
	if (ret)
		goto err_detach_device;

#if 1
	/*
	 * Attach rgb bridge to encoders needing it.
	 */
	list_for_each_entry(encoder, &drm_dev->mode_config.encoder_list, head) {
		struct device_node *rgb_node;

		if (!encoder->of_node)
			continue;

		rgb_node = of_parse_phandle(encoder->of_node,
					    "rockchip,rgb-bridge", 0);
		if (rgb_node) {
			struct drm_bridge *bridge;

			bridge = of_drm_find_bridge(rgb_node);
			of_node_put(rgb_node);
			if (!bridge) {
				ret = -EPROBE_DEFER;
				goto err_unbind;
			}

			encoder->bridge = bridge;
			bridge->encoder = encoder;
			ret = drm_bridge_attach(encoder->dev, bridge);
			if (ret) {
				DRM_ERROR("Failed to attach bridge to drm\n");
				goto err_unbind;
			}
		}
	}
#endif

	/*
	 * All components are now added, we can publish the connector sysfs
	 * entries to userspace.  This will generate hotplug events and so
	 * userspace will expect to be able to access DRM at this point.
	 */
	list_for_each_entry(connector, &drm_dev->mode_config.connector_list,
			head) {
		ret = drm_connector_register(connector);
		if (ret) {
			dev_err(drm_dev->dev,
				"[CONNECTOR:%d:%s] drm_connector_register failed: %d\n",
				connector->base.id,
				drm_get_connector_name(connector), ret);
			goto err_unbind;
		}
	}

	/* init kms poll for handling hpd */
	drm_kms_helper_poll_init(drm_dev);

	/*
	 * enable drm irq mode.
	 * - with irq_enabled = true, we can use the vblank feature.
	 */
	drm_dev->irq_enabled = true;

	ret = drm_vblank_init(drm_dev, ROCKCHIP_MAX_CRTC);
	if (ret) {
        dev_err(drm_dev->dev," %s fail\n",__FUNCTION__);
		goto err_kms_helper_poll_fini;
    }

	/*
	 * with vblank_disable_allowed = true, vblank interrupt will be disabled
	 * by drm timer once a current process gives up ownership of
	 * vblank event.(after drm_vblank_put function is called)
	 */
	drm_dev->vblank_disable_allowed = true;

	ret = rockchip_drm_fbdev_init(drm_dev);
	if (ret) {
        dev_err(drm_dev->dev," %s fail\n",__FUNCTION__);
		goto err_vblank_cleanup;
    }

    dev_err(drm_dev->dev,"rk drm load success \n");

	return 0;
err_vblank_cleanup:
	drm_vblank_cleanup(drm_dev);
err_kms_helper_poll_fini:
	drm_kms_helper_poll_fini(drm_dev);
err_unbind:
	component_unbind_all(dev, drm_dev);
err_detach_device:
	arm_iommu_detach_device(dev);
err_release_mapping:
	arm_iommu_release_mapping(dev->archdata.mapping);
err_config_cleanup:
	drm_mode_config_cleanup(drm_dev);
	drm_dev->dev_private = NULL;
	return ret;
}

static int rockchip_drm_unload(struct drm_device *drm_dev)
{
	struct device *dev = drm_dev->dev;

	drm_vblank_cleanup(drm_dev);
	drm_kms_helper_poll_fini(drm_dev);
	component_unbind_all(dev, drm_dev);
	arm_iommu_detach_device(dev);
	arm_iommu_release_mapping(dev->archdata.mapping);
	drm_mode_config_cleanup(drm_dev);
	drm_dev->dev_private = NULL;

	return 0;
}

static int rockchip_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct rockchip_drm_file_private *file_priv;

	file_priv = kzalloc(sizeof(*file_priv), GFP_KERNEL);
	if (!file_priv)
		return -ENOMEM;
	INIT_LIST_HEAD(&file_priv->gem_cpu_acquire_list);

	file->driver_priv = file_priv;

	return 0;
}

static void rockchip_drm_preclose(struct drm_device *dev,
				  struct drm_file *file)
{
	struct rockchip_drm_file_private *file_private = file->driver_priv;
	struct rockchip_gem_object_node *cur, *d;

	mutex_lock(&dev->struct_mutex);
	list_for_each_entry_safe(cur, d,
			&file_private->gem_cpu_acquire_list, list) {
#ifdef CONFIG_DRM_DMA_SYNC
		BUG_ON(!cur->rockchip_gem_obj->acquire_fence);
		drm_fence_signal_and_put(&cur->rockchip_gem_obj->acquire_fence);
#endif
		drm_gem_object_unreference(&cur->rockchip_gem_obj->base);
		kfree(cur);
	}
	/* since we are deleting the whole list, just initialize the header
	 * instead of calling list_del for every element
	 */
	INIT_LIST_HEAD(&file_private->gem_cpu_acquire_list);
	mutex_unlock(&dev->struct_mutex);
}

static void rockchip_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	kfree(file->driver_priv);
	file->driver_priv = NULL;
}

void rockchip_drm_lastclose(struct drm_device *dev)
{
	struct rockchip_drm_private *priv = dev->dev_private;

	drm_fb_helper_restore_fbdev_mode_unlocked(&priv->fbdev_helper);
}

static const struct drm_ioctl_desc rockchip_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_CREATE, rockchip_gem_create_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_MAP_OFFSET,
			  rockchip_gem_map_offset_ioctl,
			  DRM_UNLOCKED | DRM_AUTH | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_CPU_ACQUIRE,
			  rockchip_gem_cpu_acquire_ioctl,
			  DRM_UNLOCKED | DRM_AUTH),
	DRM_IOCTL_DEF_DRV(ROCKCHIP_GEM_CPU_RELEASE,
			  rockchip_gem_cpu_release_ioctl,
			  DRM_UNLOCKED | DRM_AUTH),
};

static const struct file_operations rockchip_drm_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.mmap = rockchip_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.release = drm_release,
};

const struct vm_operations_struct rockchip_drm_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static struct drm_driver rockchip_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME
					| DRIVER_RENDER,
	.load			= rockchip_drm_load,
	.unload			= rockchip_drm_unload,
	.open			= rockchip_drm_open,
	.preclose		= rockchip_drm_preclose,
	.lastclose		= rockchip_drm_lastclose,
	.postclose		= rockchip_drm_postclose,
	.get_vblank_counter	= drm_vblank_count,
	.enable_vblank		= rockchip_drm_crtc_enable_vblank,
	.disable_vblank		= rockchip_drm_crtc_disable_vblank,
	.gem_vm_ops		= &rockchip_drm_vm_ops,
	.gem_free_object	= rockchip_gem_free_object,
	.dumb_create		= rockchip_gem_dumb_create,
	.dumb_map_offset	= rockchip_gem_dumb_map_offset,
	.dumb_destroy		= drm_gem_dumb_destroy,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_import_sg_table = rockchip_gem_prime_import_sg_table,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= rockchip_gem_prime_get_sg_table,
	.gem_prime_vmap		= rockchip_gem_prime_vmap,
	.gem_prime_vunmap	= rockchip_gem_prime_vunmap,
	.gem_prime_mmap		= rockchip_gem_mmap_buf,
	.atomic_begin		= drm_atomic_begin,
	.atomic_set_event	= drm_atomic_set_event,
	.atomic_check		= drm_atomic_check,
	.atomic_commit		= drm_atomic_commit,
	.atomic_end		= drm_atomic_end,
	.atomic_funcs		= &drm_atomic_funcs,
	.ioctls			= rockchip_ioctls,
	.num_ioctls		= ARRAY_SIZE(rockchip_ioctls),
	.fops			= &rockchip_drm_driver_fops,
	.name	= DRIVER_NAME,
	.desc	= DRIVER_DESC,
	.date	= DRIVER_DATE,
	.major	= DRIVER_MAJOR,
	.minor	= DRIVER_MINOR,
};

#ifdef CONFIG_PM_SLEEP
static int rockchip_drm_sys_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct drm_connector *connector;

	if (!drm)
		return 0;

	drm_modeset_lock_all(drm);
	list_for_each_entry(connector, &drm->mode_config.connector_list, head) {
		int old_dpms = connector->dpms;

		if (connector->funcs->dpms)
			connector->funcs->dpms(connector, DRM_MODE_DPMS_OFF);

		/* Set the old mode back to the connector for resume */
		connector->dpms = old_dpms;
	}
	drm_modeset_unlock_all(drm);

	return 0;
}

static int rockchip_drm_sys_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct drm_connector *connector;
	enum drm_connector_status status;
	bool changed = false;

	if (!drm)
		return 0;

	drm_modeset_lock_all(drm);
	list_for_each_entry(connector, &drm->mode_config.connector_list, head) {
		int desired_mode = connector->dpms;

		/*
		 * at suspend time, we save dpms to connector->dpms,
		 * restore the old_dpms, and at current time, the connector
		 * dpms status must be DRM_MODE_DPMS_OFF.
		 */
		connector->dpms = DRM_MODE_DPMS_OFF;

		/*
		 * If the connector has been disconnected during suspend,
		 * disconnect it from the encoder and leave it off. We'll notify
		 * userspace at the end.
		 */
		if (desired_mode == DRM_MODE_DPMS_ON) {
			status = connector->funcs->detect(connector, true);
			if (status == connector_status_disconnected) {
				connector->encoder = NULL;
				connector->status = status;
				changed = true;
				continue;
			}
		}
		if (connector->funcs->dpms)
			connector->funcs->dpms(connector, desired_mode);
	}
	drm_modeset_unlock_all(drm);

	drm_helper_resume_force_mode(drm);

	if (changed)
		drm_kms_helper_hotplug_event(drm);

	return 0;
}
#endif

static const struct dev_pm_ops rockchip_drm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rockchip_drm_sys_suspend,
				rockchip_drm_sys_resume)
};

/*
 * @node: device tree node containing encoder input ports
 * @encoder: drm_encoder
 */
int rockchip_drm_encoder_get_mux_id(struct device_node *node,
				    struct drm_encoder *encoder)
{
	struct device_node *ep = NULL;
	struct drm_crtc *crtc = encoder->crtc;
	struct of_endpoint endpoint;
	struct device_node *port;
	int ret;

	if (!node || !crtc)
		return -EINVAL;

	do {
		ep = of_graph_get_next_endpoint(node, ep);
		if (!ep)
			break;

		port = of_graph_get_remote_port(ep);
		of_node_put(port);
		if (port == crtc->port) {
			ret = of_graph_parse_endpoint(ep, &endpoint);
			return ret ?: endpoint.id;
		}
	} while (ep);

	return -EINVAL;
}

static int compare_of(struct device *dev, void *data)
{
	struct device_node *np = data;

	return dev->of_node == np;
}

static void rockchip_add_endpoints(struct device *dev,
				   struct component_match **match,
				   struct device_node *port)
{
	struct device_node *ep, *remote;

	for_each_child_of_node(port, ep) {
		remote = of_graph_get_remote_port_parent(ep);
		if (!remote || !of_device_is_available(remote)) {
			of_node_put(remote);
			continue;
		} else if (!of_device_is_available(remote->parent)) {
			dev_warn(dev, "parent device of %s is not available\n",
				 remote->full_name);
			of_node_put(remote);
			continue;
		}

		component_match_add(dev, match, compare_of, remote);
		of_node_put(remote);
	}
}

static int rockchip_drm_bind(struct device *dev)
{
	struct drm_device *drm;
	int ret;

	drm = drm_dev_alloc(&rockchip_drm_driver, dev);
	if (!drm)
		return -ENOMEM;

	ret = drm_dev_set_unique(drm, "%s", dev_name(dev));
	if (ret)
		goto err_free;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_free;

	dev_set_drvdata(dev, drm);

	return 0;

err_free:
	drm_dev_unref(drm);
	return ret;
}

static void rockchip_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	drm_dev_unregister(drm);
	drm_dev_unref(drm);
	dev_set_drvdata(dev, NULL);
}

static const struct component_master_ops rockchip_drm_ops = {
	.bind = rockchip_drm_bind,
	.unbind = rockchip_drm_unbind,
};

static int rockchip_drm_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *np = dev->of_node;
	struct device_node *port;
	int i;

	if (!np)
		return -ENODEV;
	/*
	 * Bind the crtc ports first, so that
	 * drm_of_find_possible_crtcs called from encoder .bind callbacks
	 * works as expected.
	 */
	for (i = 0;; i++) {
		port = of_parse_phandle(np, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		component_match_add(dev, &match, compare_of, port->parent);
		of_node_put(port);
	}

	if (i == 0) {
		dev_err(dev, "missing 'ports' property\n");
		return -ENODEV;
	}

	if (!match) {
		dev_err(dev, "No available vop found for display-subsystem.\n");
		return -ENODEV;
	}
	/*
	 * For each bound crtc, bind the encoders attached to its
	 * remote endpoint.
	 */
	for (i = 0;; i++) {
		port = of_parse_phandle(np, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		rockchip_add_endpoints(dev, &match, port);
		of_node_put(port);
	}

	return component_master_add_with_match(dev, &rockchip_drm_ops, match);
}

static int rockchip_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &rockchip_drm_ops);

	return 0;
}

static const struct of_device_id rockchip_drm_dt_ids[] = {
	{ .compatible = "rockchip,display-subsystem", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rockchip_drm_dt_ids);

static struct platform_driver rockchip_drm_platform_driver = {
	.probe = rockchip_drm_platform_probe,
	.remove = rockchip_drm_platform_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "rockchip-drm",
		.of_match_table = rockchip_drm_dt_ids,
		.pm = &rockchip_drm_pm_ops,
	},
};

module_platform_driver(rockchip_drm_platform_driver);

MODULE_AUTHOR("Mark Yao <mark.yao@rock-chips.com>");
MODULE_DESCRIPTION("ROCKCHIP DRM Driver");
MODULE_LICENSE("GPL v2");
