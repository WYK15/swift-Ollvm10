import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil

class StaticInitializers(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @expectedFailureAll(archs="aarch64", oslist="linux",
                        bugnumber="https://bugs.llvm.org/show_bug.cgi?id=44053")
    def test(self):
        """ Test a static initializer. """
        self.build()

        lldbutil.run_to_source_breakpoint(self, '// break here',
                lldb.SBFileSpec("main.cpp", False))

        # We use counter to observe if the initializer was called.
        self.expect("expr counter", substrs=["(int) $", " = 0"])
        self.expect("expr -p -- struct Foo { Foo() { inc_counter(); } }; Foo f;")
        self.expect("expr counter", substrs=["(int) $", " = 1"])

    def test_failing_init(self):
        """ Test a static initializer that fails to execute. """
        self.build()

        lldbutil.run_to_source_breakpoint(self, '// break here',
                lldb.SBFileSpec("main.cpp", False))

        # FIXME: This error message is not even remotely helpful.
        self.expect("expr -p -- struct Foo2 { Foo2() { do_abort(); } }; Foo2 f;", error=True,
                    substrs=["error: couldn't run static initializer:"])
