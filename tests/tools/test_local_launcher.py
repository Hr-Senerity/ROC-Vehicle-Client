import pathlib
import subprocess
import unittest

class LauncherArguments(unittest.TestCase):
    def test_closed_argument_boundary(self):
        launcher=pathlib.Path(__file__).parents[2]/'tools'/'run-local-integration.sh'
        image='roc-vehicle-runtime:local-000000000000'
        for args in ([],['bad','/opt/roc-vehicle-it/test','--precheck'],
                     [image,'/tmp','--precheck'],[image,'/','--precheck'],
                     [image,'/opt/roc-vehicle-it/../test','--precheck'],
                     [image,'/opt/roc-vehicle-it/test','--unknown']):
            with self.subTest(args=args):
                result=subprocess.run(['bash',str(launcher),*args],capture_output=True)
                self.assertEqual(result.returncode,2)
                self.assertEqual(result.stdout,b'')

if __name__=='__main__':unittest.main()
