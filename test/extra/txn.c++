#include "mdbx.h++"

#include <iostream>

#if !defined(__cpp_lib_latch) && __cpp_lib_latch < 201907L

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;
  std::cout << "FAKE-OK (since no C++20 std::thread and/or std::latch)\n";
  return EXIT_SUCCESS;
}

#else

#include <latch>
#include <thread>

static char log_buffer[1024];

static void logger_nofmt(MDBX_log_level_t loglevel, const char *function, int line, const char *msg,
                         unsigned length) noexcept {
  (void)length;
  (void)loglevel;
  fprintf(stdout, "%s:%u %s", function, line, msg);
}

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;
  bool ok = true;
  int err;

  mdbx_setup_debug_nofmt(MDBX_LOG_VERBOSE, MDBX_DBG_ASSERT, logger_nofmt, log_buffer, sizeof(log_buffer));

  mdbx::path path = "test-txn";
  mdbx::env::remove(path);
  mdbx::env::operate_parameters operateParameters(100, 10);

  {
    mdbx::env_managed::create_parameters createParameters;
    createParameters.geometry.make_dynamic(21 * mdbx::env::geometry::MiB, 84 * mdbx::env::geometry::MiB);

    operateParameters.options.no_sticky_threads = false;
    mdbx::env_managed env(path, createParameters, operateParameters);
    auto txn = env.start_write(false);
    /* mdbx::map_handle testHandle = */ txn.create_map("xyz", mdbx::key_mode::usual, mdbx::value_mode::single);
    txn.commit();

    //-------------------------------------
    txn = env.start_write();
    MDBX_txn *c_txn = txn;
    err = mdbx_txn_reset(txn);
    assert(err == MDBX_EINVAL);
    ok = ok && err == MDBX_EINVAL;

    err = mdbx_txn_break(txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;

    err = mdbx_txn_commit(txn);
    assert(err == MDBX_RESULT_TRUE);
    ok = ok && err == MDBX_RESULT_TRUE;

    //-------------------------------------
    err = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;
    assert(c_txn == (const MDBX_txn *)txn);

    err = mdbx_txn_break(txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;

    err = mdbx_txn_reset(txn);
    assert(err == MDBX_EINVAL);
    ok = ok && err == MDBX_EINVAL;

    err = mdbx_txn_commit(txn);
    assert(err == MDBX_RESULT_TRUE);
    ok = ok && err == MDBX_RESULT_TRUE;

    err = mdbx_txn_abort(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    //-------------------------------------
    err = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;
    assert(c_txn == (const MDBX_txn *)txn);
    txn.commit();

    err = mdbx_txn_reset(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    err = mdbx_txn_break(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    err = mdbx_txn_abort(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;

    //=====================================

    txn = env.start_read();
    err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    txn.make_broken();
    err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    txn.reset_reading();
    err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    txn.abort();

    //-------------------------------------

    txn = env.start_read();
    txn.reset_reading();
    txn.make_broken();
    txn.abort();

    //=====================================

    std::latch s(1);
    txn = env.start_read();
    c_txn = txn;

    std::thread t([&]() {
      s.wait();
      err = mdbx_txn_reset(c_txn);
      assert(err == MDBX_THREAD_MISMATCH);
      ok = ok && err == MDBX_THREAD_MISMATCH;
      err = mdbx_txn_break(c_txn);
      assert(err == MDBX_THREAD_MISMATCH);
      ok = ok && err == MDBX_THREAD_MISMATCH;
      err = mdbx_txn_commit(c_txn);
      assert(err == MDBX_THREAD_MISMATCH);
      ok = ok && err == MDBX_THREAD_MISMATCH;
      err = mdbx_txn_abort(c_txn);
      assert(err == MDBX_THREAD_MISMATCH);
      ok = ok && err == MDBX_THREAD_MISMATCH;
      err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
      assert(err == MDBX_BAD_TXN);
      ok = ok && err == MDBX_BAD_TXN;
    });

    s.count_down();
    t.join();
  }

  //=====================================
  //=====================================

  {
    operateParameters.options.no_sticky_threads = true;
    operateParameters.options.nested_write_transactions = true;
    mdbx::env_managed env(path, operateParameters);

    //-------------------------------------
    auto txn = env.start_write();
    MDBX_txn *c_txn = txn;
    err = mdbx_txn_reset(txn);
    assert(err == MDBX_EINVAL);
    ok = ok && err == MDBX_EINVAL;

    err = mdbx_txn_break(txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;

    err = mdbx_txn_commit(txn);
    assert(err == MDBX_RESULT_TRUE);
    ok = ok && err == MDBX_RESULT_TRUE;

    //-------------------------------------
    err = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;
    assert(c_txn == (const MDBX_txn *)txn);

    err = mdbx_txn_break(txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;

    err = mdbx_txn_reset(txn);
    assert(err == MDBX_EINVAL);
    ok = ok && err == MDBX_EINVAL;

    err = mdbx_txn_commit(txn);
    assert(err == MDBX_RESULT_TRUE);
    ok = ok && err == MDBX_RESULT_TRUE;

    err = mdbx_txn_abort(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    //-------------------------------------
    err = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_SUCCESS);
    ok = ok && err == MDBX_SUCCESS;
    assert(c_txn == (const MDBX_txn *)txn);
    txn.commit();

    err = mdbx_txn_reset(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    err = mdbx_txn_break(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    err = mdbx_txn_abort(c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;

    //=====================================

    txn = env.start_read();
    err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    txn.make_broken();
    err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    txn.reset_reading();
    err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
    assert(err == MDBX_BAD_TXN);
    ok = ok && err == MDBX_BAD_TXN;
    txn.abort();

    //-------------------------------------

    txn = env.start_read();
    txn.reset_reading();
    txn.make_broken();
    txn.abort();

    //=====================================

    std::latch s1(1), s2(1), s3(1);
    txn = env.start_read();
    c_txn = txn;

    std::thread t([&]() {
      s1.wait();
      err = mdbx_txn_break(c_txn);
      assert(err == MDBX_SUCCESS);
      ok = ok && err == MDBX_SUCCESS;
      err = mdbx_txn_reset(c_txn);
      assert(err == MDBX_SUCCESS);
      ok = ok && err == MDBX_SUCCESS;
      txn.renew_reading();
      s2.count_down();

      s3.wait();
      err = mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &c_txn);
      assert(err == MDBX_SUCCESS);
      ok = ok && err == MDBX_SUCCESS;
      err = mdbx_txn_commit(c_txn);
      assert(err == MDBX_SUCCESS);
      ok = ok && err == MDBX_SUCCESS;
      c_txn = txn;
      err = mdbx_txn_commit(c_txn);
      assert(err == MDBX_THREAD_MISMATCH);
      ok = ok && err == MDBX_THREAD_MISMATCH;
      err = mdbx_txn_abort(c_txn);
      assert(err == MDBX_THREAD_MISMATCH);
      ok = ok && err == MDBX_THREAD_MISMATCH;
      err = mdbx_txn_break(c_txn);
      assert(err == MDBX_SUCCESS);
      ok = ok && err == MDBX_SUCCESS;
      err = mdbx_txn_reset(c_txn);
      assert(err == MDBX_EINVAL);
      ok = ok && err == MDBX_EINVAL;
    });

    s1.count_down();
    s2.wait();
    txn.commit();
    txn = env.start_write();
    s3.count_down();

    t.join();
    txn.abort();
  }

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* __cpp_lib_latch */
