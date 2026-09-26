import unittest
from training.production_commitments import ProductionCommitments, Proposal, Offer


class ProductionCommitmentTest(unittest.TestCase):
    def start(self,m=150):
        e=ProductionCommitments();e.observe(0,m,0,{1,2,3})
        e.submit(Proposal(1,'train_probe',2,240))
        return e

    def test_concurrent_demand_single_reservation_and_duplicate_proposal(self):
        e=self.start();e.submit(Proposal(2,'build_pylon',1,240))
        offers=[Offer('train_probe',1,50,0),Offer('train_probe',2,50,0),Offer('build_pylon',3,100,0)]
        tickets=e.reserve(offers)
        self.assertEqual(len(tickets),2);self.assertEqual(e.reservations(),(100,0))
        self.assertFalse(e.submit(Proposal(1,'train_probe',2,240)))
        self.assertEqual(e.reserve(offers),[])

    def test_busy_illegal_unaffordable_and_bad_placement_do_not_dispatch(self):
        e=self.start(m=40)
        self.assertEqual(e.reserve([Offer('train_probe',1,50,0)]),[])
        e.observe(24,200,0,{1,2,3})
        for field in ('idle','legal','placement'):
            self.assertEqual(e.reserve([Offer('train_probe',1,50,0,**{field:False})]),[])
        self.assertEqual(e.reservations(),(0,0))

    def test_acceptance_is_not_completion_and_spending_reconciles_once(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.accepted(t.key);e.accepted(t.key)
        self.assertEqual(e.completed(1),0);self.assertEqual(e.reservations(),(50,0))
        e.observe(24,100,0,{1,2,3},spent=[t.key])
        self.assertEqual(e.reservations(),(0,0))
        e.observe(300,200,0,{1,2,3},completed=[t.key])
        e.observe(324,200,0,{1,2,3},completed=[t.key])
        self.assertEqual(e.completed(1),1)

    def test_expiry_and_cancel_do_not_invent_dispatch_outcomes(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.cancel(1);e.observe(300,150,0,{1,2,3})
        self.assertEqual(e.reserve([Offer('train_probe',2,50,0)]),[])
        self.assertEqual(e.reservations(),(50,0));e.rejected(t.key)
        self.assertEqual(e.reservations(),(0,0))

    def test_rejected_dispatch_can_retry_but_not_more_than_twice(self):
        e=self.start();o=[Offer('train_probe',1,50,0)]
        t=e.reserve(o)[0];e.rejected(t.key);e.rejected(t.key)
        t=e.reserve(o)[0];self.assertEqual(t.slot,0);e.rejected(t.key)
        t=e.reserve(o)[0];self.assertEqual(t.slot,1)

    def test_dead_actor_does_not_release_uncertain_spending(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.accepted(t.key);e.observe(24,150,0,{2,3})
        self.assertEqual(t.phase,'uncertain');self.assertEqual(e.reservations(),(50,0))
        e.observe(48,100,0,{2,3},spent=[t.key],failed=[t.key])
        self.assertEqual(e.completed(1),0);self.assertEqual(e.reservations(),(0,0))

    def test_fallback_keeps_accepted_work_owned_until_observed_outcome(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.accepted(t.key);e.disable()
        self.assertEqual(e.reserve([Offer('train_probe',2,50,0)]),[])
        self.assertEqual(e.reservations(),(50,0))
        e.observe(24,100,0,{1,2,3},spent=[t.key],completed=[t.key])
        self.assertEqual(e.completed(1),1)

    def test_unreconciled_completion_and_conflicting_feedback_fail_atomically(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0];e.accepted(t.key)
        with self.assertRaisesRegex(ValueError,'reconciled'):
            e.observe(24,100,0,{1,2,3},completed=[t.key])
        self.assertEqual(e.frame,0);self.assertEqual(e.reservations(),(50,0))
        with self.assertRaisesRegex(ValueError,'conflicting'):
            e.observe(24,100,0,{1,2,3},spent=[t.key],completed=[t.key],failed=[t.key])

    def test_build_start_releases_builder_after_spend_but_not_completion(self):
        e=ProductionCommitments();e.observe(0,200,0,{1})
        e.submit(Proposal(1,'build_pylon',1,240));t=e.reserve([Offer('build_pylon',1,100,0)])[0]
        e.accepted(t.key);e.observe(24,100,0,{1},spent=[t.key],started=[t.key])
        self.assertFalse(t.holds_actor);self.assertEqual(e.completed(1),0)

    def test_bound_product_releases_only_legal_idle_producer(self):
        e=ProductionCommitments();e.observe(0,100,0,{1})
        e.submit(Proposal(1,'train_probe',2,240));t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.accepted(t.key);e.observe(1,50,0,{1},spent=[t.key],started=[t.key])
        self.assertEqual(e.reserve([Offer('train_probe',1,50,0,idle=False)]),[])
        self.assertEqual(len(e.reserve([Offer('train_probe',1,50,0)])),1)
        self.assertEqual(t.phase,'accepted');self.assertEqual(e.completed(1),0)

    def test_ambiguous_dispatch_can_resolve_as_unspent_rejection(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.observe(24,150,0,{2,3});self.assertEqual(t.phase,'uncertain')
        e.rejected(t.key);self.assertEqual(e.reservations(),(0,0))

    def test_accepted_dead_actor_cannot_be_relabelled_rejected(self):
        e=self.start();t=e.reserve([Offer('train_probe',1,50,0)])[0]
        e.accepted(t.key);e.observe(24,150,0,{2,3})
        with self.assertRaisesRegex(ValueError,'conflicts'):
            e.rejected(t.key)


if __name__=='__main__':unittest.main()
