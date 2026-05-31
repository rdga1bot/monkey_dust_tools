Feature: NPC behavior stability
  # Перевіряє що NPC не заморожуються, не телепортуються,
  # і правильно присутні в кадрі.

  Background:
    Given capture "latest"

  Scenario: NPCs must not freeze
    Then no NPC freezes for more than 10 ticks

  Scenario: No NPC teleportation
    Then no NPC teleports more than 15m in one tick

  Scenario: Minimum NPC count
    Then at least 3 NPCs are present

  Scenario: NPCs visible in frames
    Then NPCs are visible in at least 50% of frames
