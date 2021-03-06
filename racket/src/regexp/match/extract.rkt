#lang racket/base

;; Helpers to extract position, bytes, and string results from a match
;; result.

(provide byte-positions->byte-positions
         byte-positions->bytess
         
         byte-positions->string-positions
         byte-positions->strings
         
         add-end-bytes)

(define (byte-positions->byte-positions ms-pos me-pos state
                                        #:delta [delta 0])
  (cond
   [(not state)
    (list (cons (+ ms-pos delta) (+ me-pos delta)))]
   [(zero? delta)
    (cons (cons ms-pos me-pos) (vector->list state))]
   [else
    (cons (cons (+ ms-pos delta) (+ me-pos delta))
          (for/list ([p (in-vector state)])
            (and p
                 (cons (+ (car p) delta)
                       (+ (cdr p) delta)))))]))

(define (byte-positions->bytess in ms-pos me-pos state
                                #:delta [delta 0])
  (cons (subbytes in (+ ms-pos delta) (+ me-pos delta))
        (if state
            (for/list ([p (in-vector state)])
              (and p
                   (subbytes in (+ (car p) delta) (+ (cdr p) delta))))
            null)))

(define (byte-positions->string-positions bstr-in ms-pos me-pos state
                                          #:start-offset start-offset
                                          #:start-pos [start-pos 0])
  (define (string-offset pos)
    (+ start-offset (bytes-utf-8-length bstr-in #\? start-pos pos)))
  (cons (cons (string-offset ms-pos) (string-offset me-pos))
        (if state
            (for/list ([p (in-vector state)])
              (and p
                   (cons (string-offset (car p))
                         (string-offset (cdr p)))))
            null)))

(define (byte-positions->strings bstr-in ms-pos me-pos state
                                 #:delta [delta 0])
  (cons (bytes->string/utf-8 bstr-in #\? (+ ms-pos delta) (+ me-pos delta))
        (if state
            (for/list ([p (in-vector state)])
              (and p
                   (bytes->string/utf-8 bstr-in #\? (+ (car p) delta) (+ delta (cdr p)))))
            null)))

;; For functions like `regexp-match/end`:
(define (add-end-bytes results end-bytes-count bstr me-pos)
  (if end-bytes-count
      (values results
              (and results
                   (subbytes bstr (max 0 (- me-pos end-bytes-count)) me-pos)))
      results))
