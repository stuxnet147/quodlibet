/* C17 fixture for a call made through a declaration with no prototype.
   This cannot live in the C++ test source: empty parentheses mean zero
   parameters in C++, while C applies the default argument promotions. */

int CALLEE_unprototyped();

int ql_test_call_unprototyped(short first, float second) {
  return CALLEE_unprototyped(first, second);
}

int CALLEE_unprototyped(int first, double second) {
  return first * 7 + (int)(second * 4.0);
}
