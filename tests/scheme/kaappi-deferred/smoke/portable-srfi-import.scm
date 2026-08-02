;; Portable SRFI import smoke via --lib-path.
;; This exercises lib/srfi/111.sld rather than built-in SRFI bindings.

(import (scheme base) (scheme write) (srfi 111))

(define bx (box 41))
(set-box! bx (+ (unbox bx) 1))

(if (not (= (unbox bx) 42))
    (begin
      (display "FAIL: srfi-111 box roundtrip")
      (newline))
    (begin
      (display "all passed")
      (newline)))
