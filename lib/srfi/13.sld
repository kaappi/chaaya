(define-library (srfi 13)
  (import (scheme base))
  (export string-null?
          string-concatenate
          string-prefix?
          string-suffix?
          string-contains)
  (begin
    ;; Phase 9 partial shim: provide a focused utility subset.
    (define (string-null? s)
      (= (string-length s) 0))

    (define (string-concatenate strs)
      (apply string-append strs))

    (define (string-prefix? prefix s)
      (let ((lp (string-length prefix))
            (ls (string-length s)))
        (and (<= lp ls)
             (let loop ((i 0))
               (if (= i lp)
                   #t
                   (and (char=? (string-ref prefix i) (string-ref s i))
                        (loop (+ i 1))))))))

    (define (string-suffix? suffix s)
      (let ((lf (string-length suffix))
            (ls (string-length s)))
        (and (<= lf ls)
             (let ((offset (- ls lf)))
               (let loop ((i 0))
                 (if (= i lf)
                     #t
                     (and (char=? (string-ref suffix i)
                                  (string-ref s (+ offset i)))
                          (loop (+ i 1)))))))))

    (define (string-contains s pattern)
      (let ((ls (string-length s))
            (lp (string-length pattern)))
        (cond
          ((= lp 0) 0)
          ((> lp ls) #f)
          (else
            (let loop ((start 0))
              (if (> (+ start lp) ls)
                  #f
                  (let match ((i 0))
                    (if (= i lp)
                        start
                        (if (char=? (string-ref s (+ start i))
                                    (string-ref pattern i))
                            (match (+ i 1))
                            (loop (+ start 1)))))))))))))
