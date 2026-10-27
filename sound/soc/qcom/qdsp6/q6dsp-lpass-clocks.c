// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <linux/err.h>
#include <linux/init.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include "q6dsp-lpass-clocks.h"

#define Q6DSP_MAX_CLK_ID			104
#define Q6DSP_LPASS_CLK_ROOT_DEFAULT		0


struct q6dsp_clk {
	struct device *dev;
	int q6dsp_clk_id;
	int attributes;
	int rate;
	uint32_t handle;
	struct clk_hw hw;
};

#define to_q6dsp_clk(_hw) container_of(_hw, struct q6dsp_clk, hw)

struct q6dsp_cc {
	struct device *dev;
	struct q6dsp_clk *clks[Q6DSP_MAX_CLK_ID];
	const struct q6dsp_clk_desc *desc;
};

static int clk_q6dsp_prepare(struct clk_hw *hw)
{
	struct q6dsp_clk *clk = to_q6dsp_clk(hw);
	struct q6dsp_cc *cc = dev_get_drvdata(clk->dev);

	return cc->desc->lpass_set_clk(clk->dev, clk->q6dsp_clk_id, clk->attributes,
				     Q6DSP_LPASS_CLK_ROOT_DEFAULT, clk->rate);
}

static void clk_q6dsp_unprepare(struct clk_hw *hw)
{
	struct q6dsp_clk *clk = to_q6dsp_clk(hw);
	struct q6dsp_cc *cc = dev_get_drvdata(clk->dev);

	cc->desc->lpass_set_clk(clk->dev, clk->q6dsp_clk_id, clk->attributes,
			      Q6DSP_LPASS_CLK_ROOT_DEFAULT, 0);
}

static int clk_q6dsp_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	struct q6dsp_clk *clk = to_q6dsp_clk(hw);

	clk->rate = rate;

	return 0;
}

static unsigned long clk_q6dsp_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct q6dsp_clk *clk = to_q6dsp_clk(hw);

	return clk->rate;
}

static int clk_q6dsp_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	return 0;
}

static const struct clk_ops clk_q6dsp_ops = {
	.prepare	= clk_q6dsp_prepare,
	.unprepare	= clk_q6dsp_unprepare,
	.set_rate	= clk_q6dsp_set_rate,
	.determine_rate = clk_q6dsp_determine_rate,
	.recalc_rate	= clk_q6dsp_recalc_rate,
};

static int clk_vote_q6dsp_block(struct clk_hw *hw)
{
	struct q6dsp_clk *clk = to_q6dsp_clk(hw);
	struct q6dsp_cc *cc = dev_get_drvdata(clk->dev);

	return cc->desc->lpass_vote_clk(clk->dev, clk->q6dsp_clk_id,
				  clk_hw_get_name(&clk->hw), &clk->handle);
}

static void clk_unvote_q6dsp_block(struct clk_hw *hw)
{
	struct q6dsp_clk *clk = to_q6dsp_clk(hw);
	struct q6dsp_cc *cc = dev_get_drvdata(clk->dev);

	cc->desc->lpass_unvote_clk(clk->dev, clk->q6dsp_clk_id, clk->handle);
}

static const struct clk_ops clk_vote_q6dsp_ops = {
	.prepare	= clk_vote_q6dsp_block,
	.unprepare	= clk_unvote_q6dsp_block,
};


static struct clk_hw *q6dsp_of_clk_hw_get(struct of_phandle_args *clkspec,
					  void *data)
{
	struct q6dsp_cc *cc = data;
	unsigned int idx = clkspec->args[0];
	unsigned int attr = clkspec->args[1];

	if (idx >= Q6DSP_MAX_CLK_ID || attr > LPASS_CLK_ATTRIBUTE_INVERT_COUPLE_NO) {
		dev_err(cc->dev, "Invalid clk specifier (%d, %d)\n", idx, attr);
		return ERR_PTR(-EINVAL);
	}

	if (cc->clks[idx]) {
		cc->clks[idx]->attributes = attr;
		return &cc->clks[idx]->hw;
	}

	return ERR_PTR(-ENOENT);
}

static int q6dsp_clock_link_consumers(struct device *dev)
{
	struct of_phandle_args args;
	struct platform_device *consumer;
	struct device_node *node;
	struct device_link *link;
	bool match;
	int i;

	/* GPR services appear after remoteproc binds. fw_devlink can therefore
	 * collapse these clock dependencies to the remoteproc ancestor. That
	 * leaves live codecs using unregistered clocks after an ADSP restart.
	 * Preserve the actual DT clock supplier and let the driver core order
	 * consumer teardown and reprobe; do not hardcode a particular codec.
	 */
	for_each_node_with_property(node, "clocks") {
		if (!of_device_is_available(node))
			continue;
		match = false;
		for (i = 0; !of_parse_phandle_with_args(node, "clocks",
						      "#clock-cells", i, &args); i++) {
			match = args.np == dev->of_node;
			of_node_put(args.np);
			if (match)
				break;
		}
		if (!match)
			continue;
		consumer = of_find_device_by_node(node);
		if (!consumer)
			continue;
		link = device_link_add(&consumer->dev, dev, DL_FLAG_AUTOPROBE_CONSUMER);
		put_device(&consumer->dev);
		if (!link) {
			dev_err(dev, "failed to link clock consumer %pOF\n", node);
			of_node_put(node);
			return -EINVAL;
		}
	}
	return 0;
}

int q6dsp_clock_dev_probe(struct platform_device *pdev)
{
	struct q6dsp_cc *cc;
	struct device *dev = &pdev->dev;
	const struct q6dsp_clk_init *q6dsp_clks;
	const struct q6dsp_clk_desc *desc;
	int i, ret;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	cc->desc = desc;
	cc->dev = dev;
	q6dsp_clks = desc->clks;

	for (i = 0; i < desc->num_clks; i++) {
		unsigned int id = q6dsp_clks[i].clk_id;
		struct clk_init_data init = {
			.name =  q6dsp_clks[i].name,
		};
		struct q6dsp_clk *clk;

		clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
		if (!clk)
			return -ENOMEM;

		clk->dev = dev;
		clk->q6dsp_clk_id = q6dsp_clks[i].q6dsp_clk_id;
		clk->rate = q6dsp_clks[i].rate;
		clk->hw.init = &init;

		if (clk->rate)
			init.ops = &clk_q6dsp_ops;
		else
			init.ops = &clk_vote_q6dsp_ops;

		cc->clks[id] = clk;

		ret = devm_clk_hw_register(dev, &clk->hw);
		if (ret)
			return ret;
	}

	ret = devm_of_clk_add_hw_provider(dev, q6dsp_of_clk_hw_get, cc);
	if (ret)
		return ret;

	dev_set_drvdata(dev, cc);

	return q6dsp_clock_link_consumers(dev);
}
EXPORT_SYMBOL_GPL(q6dsp_clock_dev_probe);
