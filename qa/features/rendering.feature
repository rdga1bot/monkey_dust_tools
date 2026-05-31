Feature: Rendering correctness
  # Перевіряє небо, NPC видимість та базові рендер-артефакти.

  Background:
    Given capture "latest"

  Scenario: Sky must be visible
    Then sky is visible in all frames

  Scenario: NPCs must not be transparent
    Then NPCs are visible in at least 70% of frames
