#!/bin/bash
# Test suite for verify_alignment.py

SCRIPT="$(pwd)/../verify_alignment.py"

echo "=========================================="
echo "TESTING verify_alignment.py"
echo "=========================================="
echo


HAS_FAILED=0

# Test 1: File < 8 MiB (should pass - no boundaries to check)
echo "===== TEST 1: File < 8 MiB ====="
echo "Expected: PASS (no boundaries to check)"
python3 "$SCRIPT" test1_small.zip
result=$?
echo "Exit code: $result"
if [ $result -eq 0 ]; then
    echo "✅ TEST 1 PASSED"
else
    echo "❌ TEST 1 FAILED (unexpected)"
    HAS_FAILED=1
fi
echo
echo

# Test 2: Central directory starts before boundary (should pass)
echo "===== TEST 2: Central directory starts before 8 MiB boundary ====="
echo "Expected: PASS (boundary within central directory)"
python3 "$SCRIPT" test2_cd_before_boundary.zip
result=$?
echo "Exit code: $result"
if [ $result -eq 0 ]; then
    echo "✅ TEST 2 PASSED"
else
    echo "❌ TEST 2 FAILED (unexpected)"
    HAS_FAILED=1
fi
echo
echo

# Test 3: Random data at boundary (should fail)
echo "===== TEST 3: Random data at 8 MiB boundary ====="
echo "Expected: FAIL (invalid magic at boundary)"
python3 "$SCRIPT" test3_random_at_boundary.zip
result=$?
echo "Exit code: $result"
if [ $result -eq 1 ]; then
    echo "✅ TEST 3 PASSED (correctly detected failure)"
else
    echo "❌ TEST 3 FAILED (should have failed but returned $result)"
    HAS_FAILED=1
fi
echo
echo

# Test 4: Padding frame (0x00) at boundary (should fail)
echo "===== TEST 4: Padding skippable frame (type 0x00) at 8 MiB ====="
echo "Expected: FAIL (wrong type byte, should be 0x01)"
python3 "$SCRIPT" test4_padding_frame.zip
result=$?
echo "Exit code: $result"
if [ $result -eq 1 ]; then
    echo "✅ TEST 4 PASSED (correctly detected wrong type byte)"
else
    echo "❌ TEST 4 FAILED (should have failed but returned $result)"
    HAS_FAILED=1
fi
echo
echo

# Test 5: Start-of-Part frame (0x01) at boundary (should pass)
echo "===== TEST 5: Start-of-Part skippable frame (type 0x01) at 8 MiB ====="
echo "Expected: PASS (valid Start-of-Part frame)"
python3 "$SCRIPT" test5_start_of_part.zip
result=$?
echo "Exit code: $result"
if [ $result -eq 0 ]; then
    echo "✅ TEST 5 PASSED"
else
    echo "❌ TEST 5 FAILED (unexpected)"
    HAS_FAILED=1
fi
echo
echo

# Test 6: ZIP local file header at boundary (should pass)
echo "===== TEST 6: ZIP local file header at 8 MiB ====="
echo "Expected: PASS (valid ZIP header)"
python3 "$SCRIPT" test6_zip_header.zip
result=$?
echo "Exit code: $result"
if [ $result -eq 0 ]; then
    echo "✅ TEST 6 PASSED"
else
    echo "❌ TEST 6 FAILED (unexpected)"
    HAS_FAILED=1
fi
echo
echo

echo "=========================================="
echo "TEST SUITE COMPLETE"
echo "=========================================="
if [ $HAS_FAILED -eq 1 ]; then
    echo "❌ Some tests FAILED."
    exit 1
else
    echo "✅ All tests PASSED."
    exit 0
fi
