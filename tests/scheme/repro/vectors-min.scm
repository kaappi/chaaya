(import (scheme base) (srfi 64))
(display "before test-begin\n")
(test-begin "vectors")
(display "after test-begin\n")
(test-group "vector->list"
  (test-equal "vector->list full" '(a b c) (vector->list #(a b c))))
(test-end "vectors")
