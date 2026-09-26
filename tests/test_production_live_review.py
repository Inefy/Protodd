import unittest
from training.production_live_review import feedback_audit

class FeedbackAuditTests(unittest.TestCase):
    def test_api_success_does_not_imply_spend_or_completion(self):
        log='PRODUCTION_RESERVED,1,ticket=1,action=0\nPRODUCTION_DISPATCH,1,ticket=1,api_accepted=1\n'
        result=feedback_audit(log)
        self.assertEqual(result['spent'],0);self.assertEqual(result['completed'],0)
        self.assertFalse(result['gate_pass'])

    def test_wrong_product_lifecycle_fails(self):
        log=('PRODUCTION_RESERVED,1,ticket=1,action=0\nPRODUCTION_DISPATCH,1,ticket=1,api_accepted=1\n'
             'PRODUCTION_SPENT,7,ticket=1,product=2\nLIFECYCLE,7,create,self,3,Protoss_Probe\n'
             'PRODUCTION_COMPLETED,308,ticket=1,product=2\nLIFECYCLE,308,complete,self,3,Protoss_Probe\n')
        result=feedback_audit(log)
        self.assertEqual(len(result['issues']),2)
        self.assertFalse(result['gate_pass'])

    def test_valid_lifecycle_and_duplicate_feedback_detection(self):
        log=('PRODUCTION_RESERVED,1,ticket=1,action=0\nPRODUCTION_DISPATCH,1,ticket=1,api_accepted=1\n'
             'PRODUCTION_SPENT,7,ticket=1,product=2\nLIFECYCLE,7,create,self,2,Protoss_Probe\n'
             'PRODUCTION_COMPLETED,308,ticket=1,product=2\nLIFECYCLE,308,complete,self,2,Protoss_Probe\n')
        self.assertEqual(feedback_audit(log)['issues'],[])
        self.assertIn('duplicate completion',feedback_audit(log+'PRODUCTION_COMPLETED,309,ticket=1,product=2\n')['issues'])

if __name__=='__main__':unittest.main()
