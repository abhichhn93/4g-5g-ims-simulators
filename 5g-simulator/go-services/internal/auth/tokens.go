// Package auth provides JWT issuance, verification, and HTTP middleware.
//
// Why HMAC-SHA256 (HS256) instead of RSA (RS256)?
// HS256 uses a single shared secret — simpler to deploy (one env var) and
// perfectly adequate when a single service both issues and verifies tokens.
// RS256 is needed when multiple independent issuers exist (e.g., an
// identity provider signs, microservices verify using only the public key).
// For this simulator, all three Go services share the same JWT_SECRET, so
// HS256 is the right choice.
package auth

import (
	"errors"
	"fmt"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

// Role is a typed string rather than an int so JSON round-trips are readable.
// C++ note: this is like a scoped enum, but with string marshaling built in.
type Role string

const (
	RoleViewer   Role = "viewer"
	RoleOperator Role = "operator"
	RoleAdmin    Role = "admin"
)

// Claims embeds jwt.RegisteredClaims (standard fields like exp, iat, sub)
// and adds our application-specific fields. The jwt library reads embedded
// RegisteredClaims automatically when validating expiry, issuer, etc.
type Claims struct {
	Username string `json:"username"`
	Role     Role   `json:"role"`
	jwt.RegisteredClaims
}

// Issue creates a signed HS256 JWT for the given user. Returns the compact
// token string (header.payload.signature) ready to send in HTTP responses.
func Issue(secret, username string, role Role, expiryHours int) (string, error) {
	now := time.Now()
	claims := Claims{
		Username: username,
		Role:     role,
		RegisteredClaims: jwt.RegisteredClaims{
			Subject:   username,
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(time.Duration(expiryHours) * time.Hour)),
		},
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	signed, err := token.SignedString([]byte(secret))
	if err != nil {
		return "", fmt.Errorf("signing token: %w", err)
	}
	return signed, nil
}

// Verify parses the compact JWT string, checks the signature against secret,
// and returns the decoded claims. Returns an error if the token is expired,
// has a bad signature, or is otherwise malformed.
func Verify(secret, tokenStr string) (*Claims, error) {
	parsed, err := jwt.ParseWithClaims(tokenStr, &Claims{}, func(t *jwt.Token) (any, error) {
		// Reject non-HMAC algorithms to prevent the "alg: none" attack where
		// an attacker strips the signature and sets algorithm to "none".
		if _, ok := t.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, fmt.Errorf("unexpected signing method: %v", t.Header["alg"])
		}
		return []byte(secret), nil
	})
	if err != nil {
		return nil, fmt.Errorf("parsing token: %w", err)
	}

	claims, ok := parsed.Claims.(*Claims)
	if !ok || !parsed.Valid {
		return nil, errors.New("invalid token claims")
	}
	return claims, nil
}
