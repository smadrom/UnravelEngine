using System.Runtime.CompilerServices;

namespace Unravel.Core
{
    /// <summary>
    /// Queries against the currently loaded Perfect World map.
    /// The map terrain is a heightfield without a physics collider, so gameplay scripts
    /// sample its height directly to keep characters on the ground.
    /// </summary>
    public static class PwMap
    {
        /// <summary>True when a PW map with terrain is loaded and accepted.</summary>
        public static bool HasTerrain()
        {
            return internal_m2n_pw_map_has_terrain();
        }

        /// <summary>
        /// Samples the terrain height at a world XZ position.
        /// Returns false outside the terrain or when no map is loaded; <paramref name="height"/> is then 0.
        /// </summary>
        public static bool SampleTerrainHeight(float worldX, float worldZ, out float height)
        {
            return internal_m2n_pw_map_sample_terrain(worldX, worldZ, out height);
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern bool internal_m2n_pw_map_has_terrain();

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern bool internal_m2n_pw_map_sample_terrain(float worldX, float worldZ, out float height);
    }
}
