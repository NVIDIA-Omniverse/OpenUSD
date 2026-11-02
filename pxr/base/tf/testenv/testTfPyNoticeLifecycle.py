#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
import gc
import unittest
import weakref

from pxr import Tf


class RuntimeNotice(Tf.Notice):
    pass


class Receiver:
    def callback(self, notice, sender):
        raise AssertionError("expired receiver must never be called")


class TestNoticeLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        Tf.Type.Define(RuntimeNotice)

    def test_bound_method_does_not_retain_receiver(self):
        receiver = Receiver()
        observation = weakref.ref(receiver)
        listener = Tf.Notice.RegisterGlobally(RuntimeNotice, receiver.callback)
        del receiver
        gc.collect()
        self.assertIsNone(observation())
        RuntimeNotice().SendGlobally() # Expired callbacks warn and are skipped.
        listener.Revoke()

    def test_named_function_is_weak(self):
        def callback(notice, sender):
            self.fail("expired function must never be called")
        observation = weakref.ref(callback)
        listener = Tf.Notice.RegisterGlobally(RuntimeNotice, callback)
        del callback
        gc.collect()
        self.assertIsNone(observation())
        RuntimeNotice().SendGlobally()
        listener.Revoke()

    def test_callable_instance_is_weak(self):
        class Callable:
            def __call__(self, notice, sender):
                raise AssertionError("expired callable must never be called")
        callback = Callable()
        observation = weakref.ref(callback)
        listener = Tf.Notice.RegisterGlobally(RuntimeNotice, callback)
        del callback
        gc.collect()
        self.assertIsNone(observation())
        RuntimeNotice().SendGlobally()
        listener.Revoke()

    def test_lambda_is_retained_until_listener_destruction(self):
        delivered = []
        callback = lambda notice, sender: delivered.append((notice, sender))
        observation = weakref.ref(callback)
        listener = Tf.Notice.RegisterGlobally(RuntimeNotice, callback)
        del callback
        gc.collect()
        self.assertIsNotNone(observation())
        notice = RuntimeNotice()
        notice.SendGlobally()
        self.assertEqual(len(delivered), 1)
        self.assertIs(delivered[0][0], notice)
        self.assertIsNone(delivered[0][1])
        listener.Revoke()
        notice.SendGlobally()
        self.assertEqual(len(delivered), 1)
        # Revoke stops delivery; the Listener still owns its callback storage.
        del listener
        gc.collect()
        self.assertIsNone(observation())

    def test_nonweakrefable_callable_has_strong_fallback(self):
        events = []
        class Callable:
            __slots__ = ()
            def __call__(self, notice, sender):
                events.append("called")
            def __del__(self):
                events.append("destroyed")
        callback = Callable()
        with self.assertRaises(TypeError):
            weakref.ref(callback)
        listener = Tf.Notice.RegisterGlobally(RuntimeNotice, callback)
        del callback
        gc.collect()
        self.assertEqual(events, [])
        RuntimeNotice().SendGlobally()
        self.assertEqual(events, ["called"])
        listener.Revoke()
        del listener
        gc.collect()
        self.assertEqual(events, ["called", "destroyed"])

    def test_registration_does_not_retain_python_sender(self):
        class Sender:
            pass
        sender = Sender()
        observation = weakref.ref(sender)
        received = []
        listener = Tf.Notice.Register(RuntimeNotice,
            lambda notice, source: received.append((notice, source)), sender)
        del sender
        gc.collect()
        self.assertIsNone(observation())
        RuntimeNotice().Send(Sender())
        self.assertEqual(received, [])
        listener.Revoke()

    def test_callback_exception_preserves_type_and_recovers(self):
        delivered = []
        def callback(notice, sender):
            if not delivered:
                delivered.append("failed")
                raise ValueError("notice callback failure")
            delivered.append((notice, sender))
        listener = Tf.Notice.RegisterGlobally(RuntimeNotice, callback)
        notice = RuntimeNotice()
        with self.assertRaisesRegex(ValueError, "notice callback failure"):
            notice.SendGlobally()
        notice.SendGlobally()
        self.assertEqual(delivered, ["failed", (notice, None)])
        listener.Revoke()

    def test_invalid_callback_uses_python_type_error(self):
        with self.assertRaisesRegex(TypeError, "callable"):
            Tf.Notice.RegisterGlobally(RuntimeNotice, 42)


if __name__ == "__main__":
    unittest.main()
