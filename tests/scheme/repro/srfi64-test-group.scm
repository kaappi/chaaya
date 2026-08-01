(import (scheme base) (srfi 64))
(test-begin "vectors")
(test-group "vector literals"
  (test-equal "vector literal #(1 2 3)" #(1 2 3) #(1 2 3)))
(test-end "vectors")
