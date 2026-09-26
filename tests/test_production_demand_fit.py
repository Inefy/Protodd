import unittest
import numpy as np
import torch
from training.production_demand_fit import DemandModel, gates, scores
from training.production_demands import ACTIONS


class DemandFitTest(unittest.TestCase):
    def test_ordinal_outputs_remain_monotonic_with_gradients(self):
        model=DemandModel(torch.zeros(3),torch.ones(3),width=16)
        values=model(torch.randn(6,3))
        self.assertTrue((values[:,:,1:]<=values[:,:,:-1]).all())
        values.sum().backward()
        self.assertGreater(float(model.net[0].weight.grad.abs().sum()),0)

    def test_counts_penalize_extra_and_missing_production(self):
        y=np.zeros((2,len(ACTIONS)),dtype=np.int64);p=y.copy()
        y[0,0]=3;p[0,0]=1;p[1,1]=2
        r=scores(p,y)
        self.assertEqual(r['count_mae'],4/(2*len(ACTIONS)))
        self.assertEqual(r['per_type']['build_pylon']['precision'],0)

    def test_empty_required_type_cannot_pass(self):
        data=np.ones((20,len(ACTIONS)),dtype=np.int64);data[:,1]=0
        result=scores(data,data)
        reference=scores(np.zeros_like(data),data)
        self.assertFalse(gates(result,{'zero':reference})['checks']['build_pylon'])


if __name__=='__main__':unittest.main()
