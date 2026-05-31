Feature: Player behavior
  # Перевіряє рух гравця через QA state log.

  Background:
    Given capture "latest"

  Scenario: Player must be moving during capture
    Then player position changes at least 0.5m per second
