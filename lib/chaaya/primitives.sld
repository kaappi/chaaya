(define-library (chaaya primitives)
  (import (scheme base))
  (export %default-random-source %rs-next-int %rs-next-real)
  (include "primitives.inc.scm"))
